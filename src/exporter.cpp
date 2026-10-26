#include "exporter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

#include "builtin_fx.h"
#include "miniz.h"
#include "plugin_state.h"
#include "pugixml.hpp"
#include "rpp_reader.h"
#include "tempo_map.h"
#include "util.h"

namespace dawp {
namespace {

using pugi::xml_node;

const double kEps = 1e-9;
const int kMaster = -1;

std::string Fmt(double v) { return FormatDouble(v); }

std::string HexColor(int native)
{
  // REAPER (Windows) stores colors as 0x01bbggrr; bit 24 says the color is set.
  if (!(native & 0x1000000)) return std::string();
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", native & 0xFF, (native >> 8) & 0xFF, (native >> 16) & 0xFF);
  return buf;
}

struct EnvPoint
{
  double sec, value;
  int shape;
};

struct Receive
{
  int source;
  int mode;  // 0 post-fader, 1 pre-FX, 3 pre-fader
  double volume, pan;
  bool mute;
  int srcChannel = 0;   // first source channel (2 = channels 3/4 of a multi-output instrument, -1 = no audio)
  int destChannel = 0;  // first destination channel (2 = channels 3/4, e.g. a side-chain input)
  const RppNode* volEnv = nullptr;
  const RppNode* panEnv = nullptr;
  const RppNode* muteEnv = nullptr;
  bool consumed = false;  // became the source track's output destination
};

struct TrackOut
{
  const RppNode* node = nullptr;  // nullptr for the master
  int index = kMaster;
  std::string name;
  int parent = -2;  // enclosing folder (REAPER track index), -2 = top level
  bool folder = false, mainSend = true, hasAudio = false, hasMidi = false, instrument = false;
  std::vector<Receive> receives;
  xml_node track, channel, volume, devices;
  std::string trackId, channelId, volumeId, panId, muteId;
  xml_node lanes, clips;
};

struct MidiEvent
{
  long long tick;
  uint8_t status, d1, d2;
};

class Exporter
{
public:
  Exporter(const ExportOptions& options, ExportReport& report) : options_(options), report_(report) {}
  bool Run(const std::string& text, const std::string& outPath, std::string& error);

private:
  void Warn(const std::string& message) { warnings_[message]++; }
  std::string NewId() { return "id" + std::to_string(++nextId_); }
  double Beats(double sec) const { return tempo_.ToBeats(sec); }

  void ReadTempo();
  void CollectPools(const RppNode& n);
  void ScanTrack(TrackOut& t);
  void BuildStructure(xml_node structure);
  void BuildChannel(TrackOut& t, xml_node track, const std::string& role);
  void ExportFx(TrackOut& t, const RppNode* chain);
  void BuildRouting();
  xml_node TrackLanes(TrackOut& t);
  xml_node TrackClips(TrackOut& t);
  bool ReadEnvelope(const RppNode* env, std::vector<EnvPoint>& pts);
  void AddPoints(TrackOut& t, const std::string& paramId, const char* unit, const std::vector<EnvPoint>& pts,
                 double (*convert)(double), bool boolean);
  void ExportTrackEnvelopes(TrackOut& t);
  void ExportItem(TrackOut& t, const RppNode& item);
  void ExportAudio(TrackOut& t, xml_node clip, const std::string& file, double soffs, double rate, bool preservePitch,
                   double secStart, double secLength, double itemLen, bool loop, const std::string& sourceType);
  bool ReadMidi(const RppNode* source, int& tpq, std::vector<MidiEvent>& events, long long& endTick);
  void ExportMidi(xml_node clip, const std::vector<MidiEvent>& events, int tpq);
  void ExportMarkers(xml_node arrangement);
  void ExportTempoAutomation(xml_node arrangement);
  void WriteMetadata(pugi::xml_document& meta);
  std::string ResolvePath(const std::string& file) const;
  std::string AddMediaFile(const std::string& path);
  bool WriteZip(const std::string& outPath, const std::string& projectXml, const std::string& metaXml, std::string& error);

  const ExportOptions& options_;
  ExportReport& report_;
  std::map<std::string, int> warnings_;
  int nextId_ = 0;

  RppNode root_;
  TempoMap tempo_;
  double bpm_ = 120;
  int num_ = 4, den_ = 4;
  std::vector<EnvPoint> tempoPoints_;  // from TEMPOENVEX (shape: 0 linear, 1 square)
  std::vector<std::pair<double, int>> signatures_;  // seconds, (den << 16) | num
  std::string tempoId_, signatureId_;

  pugi::xml_document doc_;
  xml_node rootLanes_;
  TrackOut master_;
  std::vector<TrackOut> tracks_;
  std::unordered_map<std::string, const RppNode*> pools_;  // pooled MIDI: GUID -> source with events

  struct ZipEntry
  {
    std::string name;
    std::string sourcePath;      // file to copy, or
    std::vector<uint8_t> bytes;  // data
  };
  std::vector<ZipEntry> entries_;
  std::set<std::string> entryNames_;
  std::unordered_map<std::string, std::string> mediaEntries_;  // source path -> entry or external path
};

// ---------------------------------------------------------------------------------------------

void Exporter::ReadTempo()
{
  if (const RppNode* t = root_.Child("TEMPO"))
  {
    bpm_ = t->Num(1, 120);
    num_ = t->Int(2, 4);
    den_ = t->Int(3, 4);
  }
  if (!(bpm_ > 0)) bpm_ = 120;
  if (num_ < 1) num_ = 4;
  if (den_ < 1) den_ = 4;
  tempo_ = TempoMap(bpm_);

  const RppNode* env = root_.Child("TEMPOENVEX");
  if (!env) return;
  std::vector<TempoMap::Point> pts;
  for (const RppNode* p : env->Children("PT"))
  {
    TempoMap::Point tp;
    tp.time = p->Num(1);
    tp.bpm = p->Num(2, bpm_);
    tp.linear = p->Int(3, 1) == 0;
    pts.push_back(tp);
    tempoPoints_.push_back({tp.time, tp.bpm, p->Int(3, 1)});
    int sig = p->Int(4, 0);
    if (sig > 0) signatures_.push_back({tp.time, sig});
  }
  if (!pts.empty()) tempo_.Build(pts, true);
  if (!tempoPoints_.empty() && tempoPoints_[0].sec < kEps) bpm_ = tempoPoints_[0].value;
  if (!signatures_.empty() && signatures_[0].first < kEps)
  {
    num_ = signatures_[0].second & 0xFFFF;
    den_ = (signatures_[0].second >> 16) & 0xFFFF;
  }
}

void Exporter::CollectPools(const RppNode& n)
{
  for (const RppNode& c : n.children)
  {
    if (!c.block) continue;
    if (c.Name() == "SOURCE" && c.Child("HASDATA"))
      if (const RppNode* pool = c.Child("POOLEDEVTS"))
        if (!pools_.count(pool->Token(1))) pools_[pool->Token(1)] = &c;
    CollectPools(c);
  }
}

// The active take's source: lines before the first TAKE line belong to the first take,
// "TAKE SEL" marks the active one.
const RppNode* ActiveSource(const RppNode& item)
{
  std::vector<std::pair<const RppNode*, bool>> takes(1, {nullptr, false});
  for (const RppNode& c : item.children)
  {
    if (c.Name() == "TAKE") takes.push_back({nullptr, c.Token(1) == "SEL"});
    else if (c.block && c.Name() == "SOURCE") takes.back().first = &c;
  }
  for (auto& t : takes)
    if (t.second) return t.first;
  return takes[0].first;
}

std::string SourceKind(const RppNode* src)
{
  if (!src) return std::string();
  std::string type = src->Token(1);
  if (type == "SECTION")
    if (const RppNode* inner = src->Child("SOURCE")) return SourceKind(inner);
  if (type == "MIDI" || type == "MIDIPOOL") return "midi";
  if (src->Child("FILE")) return "audio";
  return std::string();
}

void Exporter::ScanTrack(TrackOut& t)
{
  const RppNode& n = *t.node;
  if (const RppNode* name = n.Child("NAME")) t.name = name->Token(1);
  if (const RppNode* ms = n.Child("MAINSEND")) t.mainSend = ms->Int(1, 1) != 0;
  for (const RppNode* item : n.Children("ITEM"))
  {
    std::string kind = SourceKind(ActiveSource(*item));
    if (kind == "midi") t.hasMidi = true;
    else if (kind == "audio") t.hasAudio = true;
  }
  if (const RppNode* chain = n.Child("FXCHAIN"))
    for (const RppNode& c : chain->children)
      if (c.block && (c.Name() == "VST" || c.Name() == "CLAP" || c.Name() == "AU" || c.Name() == "DX"))
      {
        std::string display = c.Token(1);
        size_t colon = display.find(':');
        t.instrument = colon != std::string::npos && colon > 0 && display[colon - 1] == 'i';
        break;
      }

  Receive* current = nullptr;
  for (const RppNode& c : n.children)
  {
    if (c.Name() == "AUXRECV")
    {
      t.receives.push_back({c.Int(1), c.Int(2), c.Num(3, 1), c.Num(4, 0), c.Int(5) != 0, c.Int(8, 0), c.Int(9, 0)});
      current = &t.receives.back();
    }
    else if (current && c.Name() == "AUXVOLENV") current->volEnv = &c;
    else if (current && c.Name() == "AUXPANENV") current->panEnv = &c;
    else if (current && c.Name() == "AUXMUTEENV") current->muteEnv = &c;
  }
}

void Exporter::BuildChannel(TrackOut& t, xml_node track, const std::string& role)
{
  t.track = track;
  xml_node ch = track.append_child("Channel");
  t.channel = ch;
  t.channelId = NewId();

  double vol = 1, pan = 0;
  bool mute = false, solo = false;
  int nch = 2;
  const RppNode* chain;
  if (t.index == kMaster)
  {
    if (const RppNode* v = root_.Child("MASTER_VOLUME"))
    {
      vol = v->Num(1, 1);
      pan = v->Num(2, 0);
    }
    if (const RppNode* m = root_.Child("MASTERMUTESOLO")) mute = (m->Int(1) & 1) != 0;
    if (const RppNode* c = root_.Child("MASTER_NCH")) nch = c->Int(1, 2);
    chain = root_.Child("MASTERFXLIST");
  }
  else
  {
    const RppNode& n = *t.node;
    if (const RppNode* v = n.Child("VOLPAN"))
    {
      vol = v->Num(1, 1);
      pan = v->Num(2, 0);
    }
    if (const RppNode* m = n.Child("MUTESOLO"))
    {
      mute = m->Int(1) != 0;
      solo = m->Int(2) != 0;
    }
    if (const RppNode* c = n.Child("NCHAN")) nch = c->Int(1, 2);
    chain = n.Child("FXCHAIN");
  }

  ch.append_attribute("audioChannels") = std::max(1, nch);
  ch.append_attribute("role") = role.c_str();
  if (solo) ch.append_attribute("solo") = true;
  ch.append_attribute("id") = t.channelId.c_str();

  t.devices = ch.append_child("Devices");
  ExportFx(t, chain);
  if (!t.devices.first_child()) ch.remove_child(t.devices);

  xml_node m = ch.append_child("Mute");
  m.append_attribute("value") = mute;
  t.muteId = NewId();
  m.append_attribute("id") = t.muteId.c_str();
  xml_node p = ch.append_child("Pan");
  p.append_attribute("unit") = "normalized";
  p.append_attribute("min") = "0";
  p.append_attribute("max") = "1";
  p.append_attribute("value") = Fmt((std::max(-1.0, std::min(1.0, pan)) + 1.0) / 2.0).c_str();
  t.panId = NewId();
  p.append_attribute("id") = t.panId.c_str();
  t.volume = ch.append_child("Volume");
  t.volume.append_attribute("unit") = "linear";
  t.volume.append_attribute("min") = "0";
  t.volume.append_attribute("max") = Fmt(std::max(2.0, vol)).c_str();
  t.volume.append_attribute("value") = Fmt(vol).c_str();
  t.volumeId = NewId();
  t.volume.append_attribute("id") = t.volumeId.c_str();
}

void Exporter::BuildStructure(xml_node structure)
{
  // master
  master_.name = "Master";
  xml_node mt = structure.append_child("Track");
  mt.append_attribute("contentType") = "audio";
  mt.append_attribute("loaded") = true;
  master_.trackId = NewId();
  mt.append_attribute("id") = master_.trackId.c_str();
  mt.append_attribute("name") = "Master";
  if (const RppNode* c = root_.Child("MASTERPEAKCOL"))
  {
    std::string color = HexColor(c->Int(1));
    if (!color.empty()) mt.append_attribute("color") = color.c_str();
  }
  BuildChannel(master_, mt, "master");

  std::vector<xml_node> parents{structure};
  std::vector<int> parentIdx{-2};
  for (TrackOut& t : tracks_)
  {
    const RppNode& n = *t.node;
    int busType = 0, busDelta = 0;
    if (const RppNode* b = n.Child("ISBUS"))
    {
      busType = b->Int(1);
      busDelta = b->Int(2);
    }
    t.folder = busType == 1;
    t.parent = parentIdx.back();

    xml_node tr = parents.back().append_child("Track");
    std::string content;
    if (t.folder) content = "tracks";
    if (t.hasAudio) content += content.empty() ? "audio" : " audio";
    if (t.hasMidi) content += content.empty() ? "notes" : " notes";
    if (content.empty()) content = t.instrument ? "notes" : "audio";
    tr.append_attribute("contentType") = content.c_str();
    tr.append_attribute("loaded") = true;
    t.trackId = NewId();
    tr.append_attribute("id") = t.trackId.c_str();
    tr.append_attribute("name") = t.name.c_str();
    if (const RppNode* c = n.Child("PEAKCOL"))
    {
      std::string color = HexColor(c->Int(1));
      if (!color.empty()) tr.append_attribute("color") = color.c_str();
    }
    std::string role = "regular";
    if (t.folder) role = "submix";
    else if (!t.hasAudio && !t.hasMidi && !t.receives.empty()) role = "effect";
    BuildChannel(t, tr, role);

    if (t.folder)
    {
      parents.push_back(tr);
      parentIdx.push_back(t.index);
    }
    else if (busType == 2)
      for (int i = 0; i < -busDelta && parents.size() > 1; i++)
      {
        parents.pop_back();
        parentIdx.pop_back();
      }
  }
}

void Exporter::BuildRouting()
{
  // A track without master/parent send outputs to one of its plain receives (post-fader, unity,
  // from and into channels 1/2, no envelopes): the inverse of how the importer routes to non-parent buses.
  auto plain = [](const Receive& r) {
    return r.mode == 0 && std::fabs(r.volume - 1) < 1e-6 && std::fabs(r.pan) < 1e-6 && !r.mute && !r.volEnv &&
           !r.panEnv && !r.muteEnv && r.srcChannel == 0 && r.destChannel == 0;
  };
  // tracks without main send that feed a track plainly: a bus collects several of them, while a
  // parallel effect (doubler, reverb) is usually fed by one
  std::vector<int> feeders(tracks_.size());
  for (size_t i = 0; i < tracks_.size(); i++)
    for (const Receive& r : tracks_[i].receives)
      if (plain(r) && r.source >= 0 && r.source < (int)tracks_.size() && !tracks_[r.source].mainSend) feeders[i]++;
  auto feedsPlainly = [&](int from, int to) {
    if (tracks_[from].mainSend) return false;
    for (const Receive& r : tracks_[to].receives)
      if (r.source == from && plain(r)) return true;
    return false;
  };

  for (TrackOut& t : tracks_)
  {
    if (t.mainSend) continue;
    std::vector<std::pair<Receive*, int>> candidates;  // receive, destination track index
    for (size_t i = 0; i < tracks_.size(); i++)
      for (Receive& r : tracks_[i].receives)
        if (r.source == t.index && plain(r)) candidates.push_back({&r, (int)i});
    if (candidates.empty())
    {
      Warn("'" + t.name + "' has its master/parent send off and no unity post-fader send, so it has no main output");
      continue;
    }
    // When a track sends to several buses, the output is the one the others lead to (vocal ->
    // doubler -> vox bus and vocal -> vox bus: vox bus), else the one most tracks output to. Ties
    // (vocals sending at 0 dB to a vox bus and a reverb): a bus gets only plain receives from
    // tracks without main send, while effect returns also get sends at other levels or from tracks
    // that go to the master; last, effect-like names lose.
    auto effectName = [&](int d) {
      std::string n = tracks_[d].name;
      for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
      for (const char* w : {"verb", "delay", "echo", "room", "hall", "plate", "chorus", "doubl", "spring", "slap",
                            "throw", "return", "fx"})
        if (n.find(w) != std::string::npos) return true;
      return false;
    };
    auto score = [&](int d) {
      int lead = 0, other = 0;
      for (const auto& c : candidates)
        if (c.second != d && feedsPlainly(c.second, d)) lead++;
      for (const Receive& r : tracks_[d].receives)
        if (!plain(r) || r.source < 0 || r.source >= (int)tracks_.size() || tracks_[r.source].mainSend) other++;
      return std::make_tuple(lead, feeders[d], -other, !effectName(d));
    };
    auto best = candidates.front();
    bool tie = false;
    for (const auto& c : candidates)
    {
      if (c.second == best.second) continue;
      if (score(c.second) > score(best.second)) best = c, tie = false;
      else if (score(c.second) == score(best.second)) tie = true;
    }
    if (tie)
      Warn("'" + t.name + "' sends at 0 dB to several tracks that look alike; its output is '" +
           tracks_[best.second].name + "', the others are sends");
    best.first->consumed = true;
    TrackOut& d = tracks_[best.second];
    t.channel.append_attribute("destination") = d.channelId.c_str();
    // a channel that tracks output to is a bus (submix), not an effect return: Studio One cannot
    // route track outputs to effect channels and drops the bus's devices
    d.channel.attribute("role").set_value("submix");
  }
  for (TrackOut& t : tracks_)
  {
    if (!t.mainSend) continue;
    const std::string& dest = t.parent >= 0 ? tracks_[t.parent].channelId : master_.channelId;
    t.channel.append_attribute("destination") = dest.c_str();
  }

  // Receives become sends on the source channel (inserted before <Volume>, as the schema wants).
  for (TrackOut& d : tracks_)
    for (Receive& r : d.receives)
    {
      if (r.consumed) continue;
      if (r.source < 0 || r.source >= (int)tracks_.size() || r.source == d.index)
      {
        Warn("A send from an unknown track was skipped");
        continue;
      }
      TrackOut& src = tracks_[r.source];
      xml_node sends = src.channel.child("Sends");
      if (!sends) sends = src.channel.insert_child_before("Sends", src.volume);
      xml_node s = sends.append_child("Send");
      s.append_attribute("destination") = d.channelId.c_str();
      s.append_attribute("type") = r.mode == 0 ? "post" : "pre";
      if (r.mode == 1) Warn("Pre-FX sends were exported as pre-fader sends");
      if (r.srcChannel != 0)
        Warn("Sends from other channels than 1/2 (multi-output instruments, MIDI-only sends) were exported as sends of the whole track");
      s.append_attribute("id") = NewId().c_str();
      xml_node en = s.append_child("Enable");
      en.append_attribute("value") = !r.mute;
      std::string enId = NewId();
      en.append_attribute("id") = enId.c_str();
      xml_node pan = s.append_child("Pan");
      pan.append_attribute("unit") = "normalized";
      pan.append_attribute("min") = "0";
      pan.append_attribute("max") = "1";
      pan.append_attribute("value") = Fmt((std::max(-1.0, std::min(1.0, r.pan)) + 1.0) / 2.0).c_str();
      std::string panId = NewId();
      pan.append_attribute("id") = panId.c_str();
      xml_node vol = s.append_child("Volume");
      vol.append_attribute("unit") = "linear";
      vol.append_attribute("min") = "0";
      vol.append_attribute("max") = Fmt(std::max(2.0, r.volume)).c_str();
      vol.append_attribute("value") = Fmt(r.volume).c_str();
      std::string volId = NewId();
      vol.append_attribute("id") = volId.c_str();

      std::vector<EnvPoint> pts;
      if (ReadEnvelope(r.volEnv, pts)) AddPoints(src, volId, "linear", pts, [](double v) { return v; }, false);
      if (ReadEnvelope(r.panEnv, pts)) AddPoints(src, panId, "normalized", pts, [](double v) { return (1.0 - v) / 2.0; }, false);
      if (ReadEnvelope(r.muteEnv, pts)) AddPoints(src, enId, nullptr, pts, [](double v) { return v; }, true);
    }
}

// ---------------------------------------------------------------------------------------------
// FX

void Exporter::ExportFx(TrackOut& t, const RppNode* chain)
{
  if (!chain) return;
  bool bypass = false, offline = false;
  int fxIndex = -1;
  xml_node device, params;
  bool isJs = false, isVst2 = false;
  BuiltinKind kind = BuiltinKind::None;
  std::vector<std::string> sliderIds;
  std::string deviceName;

  for (const RppNode& c : chain->children)
  {
    const std::string& name = c.Name();
    if (name == "BYPASS")
    {
      bypass = c.Int(1) != 0;
      offline = c.Int(2) != 0;
      continue;
    }
    static const char* fxBlocks[] = {"VST", "CLAP", "JS", "AU", "DX", "LV2", "VIDEO_EFFECT", "CONTAINER"};
    bool isFx = false;
    for (const char* b : fxBlocks)
      if (c.block && name == b) isFx = true;

    if (isFx)
    {
      fxIndex++;
      device = params = xml_node();
      isJs = isVst2 = false;
      kind = BuiltinKind::None;
      deviceName = c.Token(1);

      if (name == "VST" || name == "CLAP")
      {
        std::vector<std::string> lines;
        if (name == "VST")
        {
          for (const RppNode& l : c.children)
            if (!l.block) lines.push_back(l.line);
        }
        else if (const RppNode* st = c.Child("STATE"))
          for (const RppNode& l : st->children) lines.push_back(l.line);

        ReaperPluginState ps;
        std::string err;
        if (!ReadReaperPlugin(c.tokens, lines, ps, err))
        {
          Warn("Plug-in '" + deviceName + "' was not exported: " + err);
          continue;
        }
        if (!err.empty()) Warn("Plug-in '" + deviceName + "' was exported without its settings: " + err);
        const char* element = ps.info.format == PluginFormat::Vst3 ? "Vst3Plugin"
                              : ps.info.format == PluginFormat::Vst2 ? "Vst2Plugin" : "ClapPlugin";
        isVst2 = ps.info.format == PluginFormat::Vst2;
        deviceName = ps.info.name;
        device = t.devices.append_child(element);
        device.append_attribute("deviceID") = ps.info.id.c_str();
        device.append_attribute("deviceName") = ps.info.name.c_str();
        device.append_attribute("deviceRole") = ps.info.instrument ? "instrument" : "audioFX";
        if (!ps.info.vendor.empty()) device.append_attribute("deviceVendor") = ps.info.vendor.c_str();
        device.append_attribute("loaded") = !offline;
        device.append_attribute("id") = NewId().c_str();
        params = device.append_child("Parameters");
        xml_node en = device.append_child("Enabled");
        en.append_attribute("value") = !bypass;
        en.append_attribute("id") = NewId().c_str();
        if (!ps.state.empty())
        {
          std::string safe;
          for (char ch : ps.info.name) safe += (isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '_';
          std::string entry = "plugins/" + safe + "-" + std::to_string(entries_.size() + 1) + "." + ps.fileExtension;
          entries_.push_back({entry, std::string(), std::move(ps.state)});
          device.append_child("State").append_attribute("path") = entry.c_str();
        }
        report_.plugins++;
      }
      else if (name == "JS")
      {
        kind = JsfxKind(c.Token(1));
        if (kind == BuiltinKind::None)
        {
          Warn("JSFX effects have no DAWproject equivalent and were not exported (e.g. '" + c.Token(1) + "')");
          continue;
        }
        std::vector<double> sliders;
        for (const RppNode& l : c.children)
        {
          if (l.block) continue;
          for (const std::string& tok : l.tokens)
          {
            double v;
            sliders.push_back(ParseDouble(tok.c_str(), v) ? v : std::nan(""));
          }
          break;
        }
        int count = JsfxSliderCount(kind);
        std::vector<double> defaults;
        sliders.resize(count, std::nan(""));
        for (int i = 0; i < count; i++)
          if (std::isnan(sliders[i]))
          {
            double mn, mx;
            JsfxSliderRange(kind, i, mn, mx);
            sliders[i] = mn;  // unset slider; should not happen for our own effects
          }
        device = WriteBuiltinDevice(kind, sliders, !bypass, !offline, t.devices, [this] { return NewId(); }, sliderIds);
        isJs = true;
        report_.plugins++;
      }
      else
        Warn(name + " plug-ins cannot be stored in a DAWproject and were not exported (e.g. '" + deviceName + "')");
      continue;
    }

    if (name == "PARMENV" && c.block && device)
    {
      std::string ref = c.Token(1);
      int param = atoi(ref.c_str());
      std::vector<EnvPoint> pts;
      if (!ReadEnvelope(&c, pts)) continue;
      if (isJs)
      {
        if (param < 0 || param >= (int)sliderIds.size() || sliderIds[param].empty())
        {
          Warn("Automation of equalizer band type/order was not exported");
          continue;
        }
        std::string unit;
        BuiltinParamValue(kind, param, 0, unit);
        for (EnvPoint& p : pts) p.value = BuiltinParamValue(kind, param, p.value, unit);
        AddPoints(t, sliderIds[param], unit.c_str(), pts, [](double v) { return v; }, false);
        continue;
      }
      std::string ident;
      if (isVst2) ident = std::to_string(param);
      else if (options_.paramIdent) ident = options_.paramIdent(t.index, fxIndex, param);
      long long pid;
      char* end = nullptr;
      pid = ident.empty() ? 0 : strtoll(ident.c_str(), &end, 10);
      if (ident.empty() || (end && *end))
      {
        Warn("VST3/CLAP parameter automation can only be exported from inside REAPER (e.g. on '" + deviceName + "')");
        continue;
      }
      std::string id;
      for (xml_node p : params.children("RealParameter"))
        if (p.attribute("parameterID").as_llong() == pid) id = p.attribute("id").value();
      if (id.empty())
      {
        id = NewId();
        xml_node p = params.append_child("RealParameter");
        p.append_attribute("parameterID") = (int)pid;
        p.append_attribute("unit") = "normalized";
        p.append_attribute("min") = "0";
        p.append_attribute("max") = "1";
        p.append_attribute("value") = Fmt(pts[0].value).c_str();
        p.append_attribute("id") = id.c_str();
      }
      AddPoints(t, id, "normalized", pts, [](double v) { return std::max(0.0, std::min(1.0, v)); }, false);
    }
  }
  // empty <Parameters/> are fine for the schema but noise for readers
  for (xml_node d : t.devices.children())
    if (xml_node p = d.child("Parameters"))
      if (!p.first_child()) d.remove_child(p);
}

// ---------------------------------------------------------------------------------------------
// Automation

bool Exporter::ReadEnvelope(const RppNode* env, std::vector<EnvPoint>& pts)
{
  pts.clear();
  if (!env) return false;
  if (const RppNode* act = env->Child("ACT"))
    if (act->Int(1, 1) == 0) return false;  // bypassed envelope
  if (env->Child("POOLEDENVINST")) Warn("Automation items were not exported");
  for (const RppNode* p : env->Children("PT"))
  {
    int shape = p->Int(3, 0);
    if (shape > 1) Warn("Curved envelope segments were exported as linear");
    pts.push_back({p->Num(1), p->Num(2), shape});
  }
  std::stable_sort(pts.begin(), pts.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.sec < b.sec; });
  return !pts.empty();
}

xml_node Exporter::TrackLanes(TrackOut& t)
{
  if (!t.lanes)
  {
    t.lanes = rootLanes_.append_child("Lanes");
    t.lanes.append_attribute("track") = t.trackId.c_str();
    t.lanes.append_attribute("id") = NewId().c_str();
  }
  return t.lanes;
}

xml_node Exporter::TrackClips(TrackOut& t)
{
  if (!t.clips)
  {
    xml_node lanes = TrackLanes(t);
    t.clips = lanes.first_child() ? lanes.insert_child_before("Clips", lanes.first_child()) : lanes.append_child("Clips");
    t.clips.append_attribute("id") = NewId().c_str();
  }
  return t.clips;
}

void Exporter::AddPoints(TrackOut& t, const std::string& paramId, const char* unit, const std::vector<EnvPoint>& pts,
                         double (*convert)(double), bool boolean)
{
  xml_node points = TrackLanes(t).append_child("Points");
  points.append_attribute("id") = NewId().c_str();
  if (unit) points.append_attribute("unit") = unit;
  points.append_child("Target").append_attribute("parameter") = paramId.c_str();
  for (const EnvPoint& p : pts)
  {
    xml_node pt = points.append_child(boolean ? "BoolPoint" : "RealPoint");
    pt.append_attribute("time") = Fmt(Beats(p.sec)).c_str();
    if (boolean)
      pt.append_attribute("value") = convert(p.value) > 0.5;
    else
    {
      pt.append_attribute("value") = Fmt(convert(p.value)).c_str();
      pt.append_attribute("interpolation") = p.shape == 1 ? "hold" : "linear";
    }
  }
}

void Exporter::ExportTrackEnvelopes(TrackOut& t)
{
  const RppNode* n = t.node ? t.node : &root_;
  const char* volTag = t.node ? "VOLENV2" : "MASTERVOLENV2";
  const char* panTag = t.node ? "PANENV2" : "MASTERPANENV2";
  std::vector<EnvPoint> pts;
  if (ReadEnvelope(n->Child(volTag), pts)) AddPoints(t, t.volumeId, "linear", pts, [](double v) { return v; }, false);
  // REAPER pan envelopes run from +1 (left) to -1 (right)
  if (ReadEnvelope(n->Child(panTag), pts))
    AddPoints(t, t.panId, "normalized", pts, [](double v) { return (1.0 - std::max(-1.0, std::min(1.0, v))) / 2.0; }, false);
  // REAPER mute envelopes: 1 = playing, 0 = muted
  if (t.node && ReadEnvelope(n->Child("MUTEENV"), pts)) AddPoints(t, t.muteId, nullptr, pts, [](double v) { return v < 0.5 ? 1.0 : 0.0; }, true);
  if (ReadEnvelope(n->Child(t.node ? "VOLENV" : "MASTERVOLENV"), pts)) Warn("Pre-FX volume envelopes were not exported");
  if (ReadEnvelope(n->Child(t.node ? "PANENV" : "MASTERPANENV"), pts)) Warn("Pre-FX pan envelopes were not exported");
  if (ReadEnvelope(n->Child(t.node ? "WIDTHENV2" : "MASTERWIDTHENV2"), pts)) Warn("Width envelopes were not exported");
}

// ---------------------------------------------------------------------------------------------
// Items

std::string Exporter::ResolvePath(const std::string& file) const
{
  if (file.empty() || IsAbsolutePath(file)) return NativeSeparators(file);
  for (const std::string& dir : options_.mediaDirs)
  {
    std::string p = NativeSeparators(JoinPath(dir, file));
    if (FileExists(p)) return p;
  }
  return NativeSeparators(JoinPath(options_.mediaDirs.empty() ? std::string(".") : options_.mediaDirs[0], file));
}

std::string Exporter::AddMediaFile(const std::string& path)
{
  auto it = mediaEntries_.find(path);
  if (it != mediaEntries_.end()) return it->second;
  std::string ref;
  if (!options_.embedMedia || !FileExists(path))
    ref = path;  // external reference
  else
  {
    std::string name = FileName(path), stem = FileStem(path), ext = LowerExtension(path);
    std::string entry = "audio/" + name;
    for (int n = 2; entryNames_.count(ToLower(entry)); n++) entry = "audio/" + stem + "-" + std::to_string(n) + "." + ext;
    entryNames_.insert(ToLower(entry));
    entries_.push_back({entry, path, {}});
    report_.mediaFiles++;
    ref = entry;
  }
  mediaEntries_[path] = ref;
  return ref;
}

void Exporter::ExportItem(TrackOut& t, const RppNode& item)
{
  double pos = 0, len = 0, fadeIn = 0, fadeOut = 0;
  bool loop = false, mute = false;
  if (const RppNode* n = item.Child("POSITION")) pos = n->Num(1);
  if (const RppNode* n = item.Child("LENGTH")) len = n->Num(1);
  if (const RppNode* n = item.Child("LOOP")) loop = n->Int(1) != 0;
  if (const RppNode* n = item.Child("MUTE")) mute = n->Int(1) != 0;
  // FADEIN shape length auto-length ...: an automatic crossfade overrides the manual length
  if (const RppNode* n = item.Child("FADEIN")) fadeIn = n->Num(3) > 0 ? n->Num(3) : n->Num(2);
  if (const RppNode* n = item.Child("FADEOUT")) fadeOut = n->Num(3) > 0 ? n->Num(3) : n->Num(2);
  if (const RppNode* n = item.Child("VOLPAN"))
    if (std::fabs(n->Num(1, 1) - 1) > 1e-6) Warn("Item volume was not exported (DAWproject clips have no gain)");
  if (!(len > kEps)) return;

  // The active take's properties: lines before the first TAKE belong to take 1.
  struct Take
  {
    std::string name;
    double soffs = 0, rate = 1;
    bool preservePitch = true;
    const RppNode* source = nullptr;
    bool selected = false;
  };
  std::vector<Take> takes(1);
  for (const RppNode& c : item.children)
  {
    const std::string& name = c.Name();
    if (name == "TAKE")
    {
      takes.emplace_back();
      takes.back().selected = c.Token(1) == "SEL";
    }
    else if (name == "NAME") takes.back().name = c.Token(1);
    else if (name == "SOFFS") takes.back().soffs = c.Num(1);
    else if (name == "PLAYRATE")
    {
      takes.back().rate = c.Num(1, 1);
      takes.back().preservePitch = c.Int(2, 1) != 0;
    }
    else if (c.block && name == "SOURCE") takes.back().source = &c;
    else if (c.block && name == "TAKEFX") Warn("Take FX were not exported");
  }
  Take take = takes[0];
  for (const Take& tk : takes)
    if (tk.selected) take = tk;
  if (takes.size() > 1) Warn("Only the active take of items with several takes was exported");
  if (!take.source) return;
  if (!(take.rate > kEps)) take.rate = 1;

  xml_node clip = TrackClips(t).append_child("Clip");
  double b0 = Beats(pos), b1 = Beats(pos + len);
  clip.append_attribute("time") = Fmt(b0).c_str();
  clip.append_attribute("duration") = Fmt(b1 - b0).c_str();
  if (!take.name.empty()) clip.append_attribute("name") = take.name.c_str();
  if (const RppNode* c = item.Child("COLOR"))
  {
    std::string color = HexColor(c->Int(1));
    if (!color.empty()) clip.append_attribute("color") = color.c_str();
  }

  const RppNode* src = take.source;
  double secStart = 0, secLength = 0;
  std::string type = src->Token(1);
  if (type == "SECTION")
  {
    if (const RppNode* n = src->Child("STARTPOS")) secStart = n->Num(1);
    if (const RppNode* n = src->Child("LENGTH")) secLength = n->Num(1);
    if (const RppNode* n = src->Child("MODE"))
      if (n->Int(1) & 2) Warn("Reversed items were exported playing forwards");
    if (const RppNode* inner = src->Child("SOURCE"))
    {
      src = inner;
      type = src->Token(1);
    }
  }

  if (type == "MIDI" || type == "MIDIPOOL")
  {
    int tpq = 960;
    std::vector<MidiEvent> events;
    long long endTick = 0;
    if (!ReadMidi(src, tpq, events, endTick))
    {
      Warn("A MIDI item without readable data was skipped");
      TrackClips(t).remove_child(clip);
      return;
    }
    if (std::fabs(take.rate - 1) > 1e-6) Warn("Playback rate of MIDI items was not exported");
    double srcLen = (double)endTick / tpq;
    double playStart = take.soffs != 0 ? b0 - Beats(pos - take.soffs) : 0;
    clip.append_attribute("contentTimeUnit") = "beats";
    if (std::fabs(playStart) > kEps) clip.append_attribute("playStart") = Fmt(playStart).c_str();
    if (loop && srcLen > kEps && playStart + (b1 - b0) > srcLen + 1.0 / tpq)
    {
      clip.append_attribute("loopStart") = "0";
      clip.append_attribute("loopEnd") = Fmt(srcLen).c_str();
    }
    if (mute) clip.append_attribute("enable") = false;
    if (fadeIn > kEps || fadeOut > kEps) Warn("Fades on MIDI items were not exported");
    ExportMidi(clip, events, tpq);
  }
  else if (const RppNode* file = src->Child("FILE"))
  {
    clip.append_attribute("contentTimeUnit") = "seconds";
    clip.append_attribute("fadeTimeUnit") = "seconds";
    if (fadeIn > kEps) clip.append_attribute("fadeInTime") = Fmt(std::min(fadeIn, len)).c_str();
    if (fadeOut > kEps) clip.append_attribute("fadeOutTime") = Fmt(std::min(fadeOut, len)).c_str();
    if (mute) clip.append_attribute("enable") = false;
    ExportAudio(t, clip, file->Token(1), take.soffs, take.rate, take.preservePitch, secStart, secLength, len, loop, type);
  }
  else
  {
    if (type != "EMPTY") Warn("Items with " + type + " sources were not exported");
    TrackClips(t).remove_child(clip);
    return;
  }
  report_.clips++;
}

void Exporter::ExportAudio(TrackOut& t, xml_node clip, const std::string& file, double soffs, double rate,
                           bool preservePitch, double secStart, double secLength, double itemLen, bool loop,
                           const std::string& sourceType)
{
  (void)t;
  std::string path = ResolvePath(file);
  MediaInfo mi;
  bool known = ProbeAudioFile(path, mi) || (options_.probeMedia && options_.probeMedia(path, mi));
  if (!FileExists(path)) Warn("Media file not found, referenced by path only: " + path);
  else if (!known) Warn("Could not read the format of " + FileName(path) + " (assumed 48 kHz stereo)");
  if (!known || !(mi.duration > 0))
  {
    mi.duration = std::max(mi.duration, secStart + soffs + itemLen * rate);
    if (mi.sampleRate <= 0) mi.sampleRate = 48000;
    if (mi.channels <= 0) mi.channels = 2;
  }
  double srcLen = secLength > kEps ? secLength : mi.duration - secStart;

  // Clip content time is in seconds; with a playback rate the audio is warped by that rate.
  bool warped = std::fabs(rate - 1) > 1e-9;
  double scale = 1.0 / rate;
  double playStart = (secStart + soffs) * scale;
  if (std::fabs(playStart) > kEps) clip.append_attribute("playStart") = Fmt(playStart).c_str();
  // a loop that would only repeat a sliver (sample rounding of glued items) is not a loop
  if (loop && soffs + itemLen * rate > srcLen + 0.001)
  {
    clip.append_attribute("loopStart") = Fmt(secStart * scale).c_str();
    clip.append_attribute("loopEnd") = Fmt((secStart + srcLen) * scale).c_str();
  }

  xml_node parent = clip;
  if (warped)
  {
    parent = clip.append_child("Warps");
    parent.append_attribute("contentTimeUnit") = "seconds";
    parent.append_attribute("id") = NewId().c_str();
  }
  xml_node audio = parent.append_child(sourceType == "VIDEO" ? "Video" : "Audio");
  if (warped) audio.append_attribute("algorithm") = preservePitch ? "stretch" : "repitch";
  audio.append_attribute("channels") = mi.channels;
  audio.append_attribute("duration") = Fmt(mi.duration).c_str();
  audio.append_attribute("sampleRate") = mi.sampleRate;
  audio.append_attribute("id") = NewId().c_str();
  std::string ref = AddMediaFile(path);
  xml_node f = audio.append_child("File");
  f.append_attribute("path") = ref.c_str();
  if (ref == path) f.append_attribute("external") = true;
  if (warped)
  {
    xml_node w0 = parent.append_child("Warp");
    w0.append_attribute("time") = "0";
    w0.append_attribute("contentTime") = "0";
    xml_node w1 = parent.append_child("Warp");
    w1.append_attribute("time") = Fmt(mi.duration * scale).c_str();
    w1.append_attribute("contentTime") = Fmt(mi.duration).c_str();
  }
}

bool Exporter::ReadMidi(const RppNode* source, int& tpq, std::vector<MidiEvent>& events, long long& endTick)
{
  const RppNode* src = source;
  if (!src->Child("HASDATA"))
    if (const RppNode* pool = src->Child("POOLEDEVTS"))
    {
      auto it = pools_.find(pool->Token(1));
      if (it != pools_.end()) src = it->second;
    }
  const RppNode* hasData = src->Child("HASDATA");
  if (!hasData) return false;
  tpq = hasData->Int(2, 960);
  if (tpq <= 0) tpq = 960;
  if (const RppNode* ign = src->Child("IGNTEMPO"))
    if (ign->Int(1) != 0) Warn("MIDI items that ignore the project tempo were exported following it");

  long long tick = 0;
  for (const RppNode& c : src->children)
  {
    const std::string& name = c.Name();
    if (name.empty()) continue;
    char kind = name[0];
    if (kind != 'E' && kind != 'e' && kind != 'X' && kind != 'x') continue;
    if (name.size() > 1 && name.find_first_not_of("EeXxm") != std::string::npos) continue;
    tick += strtoll(c.Token(1).c_str(), nullptr, 10);
    if (kind == 'X' || kind == 'x')
    {
      Warn("MIDI SysEx and text events were not exported");
      continue;
    }
    if (name.find('m') != std::string::npos)
    {
      Warn("Muted MIDI events were not exported");
      continue;
    }
    MidiEvent e;
    e.tick = tick;
    e.status = (uint8_t)strtoul(c.Token(2).c_str(), nullptr, 16);
    e.d1 = (uint8_t)strtoul(c.Token(3).c_str(), nullptr, 16);
    e.d2 = (uint8_t)strtoul(c.Token(4).c_str(), nullptr, 16);
    events.push_back(e);
  }
  endTick = tick;
  // REAPER ends every MIDI source with "all notes off" on channel 1, marking the source length.
  if (!events.empty() && events.back().status == 0xB0 && events.back().d1 == 0x7B && events.back().tick == endTick)
    events.pop_back();
  return true;
}

void Exporter::ExportMidi(xml_node clip, const std::vector<MidiEvent>& events, int tpq)
{
  struct Note
  {
    long long start, end = -1;
    int ch, key, vel, rel = 64;
  };
  std::vector<Note> notes;
  std::map<int, std::vector<size_t>> open;  // (ch << 7 | key) -> open notes, oldest first
  struct CcKey
  {
    int status, ch, num;
    bool operator<(const CcKey& o) const
    {
      return status != o.status ? status < o.status : ch != o.ch ? ch < o.ch : num < o.num;
    }
  };
  std::map<CcKey, std::vector<std::pair<long long, double>>> ccs;
  long long last = 0;
  for (const MidiEvent& e : events)
  {
    last = std::max(last, e.tick);
    int type = e.status & 0xF0, ch = e.status & 0x0F;
    if (type == 0x90 && e.d2 > 0)
    {
      notes.push_back({e.tick, -1, ch, e.d1 & 0x7F, e.d2 & 0x7F});
      open[(ch << 7) | (e.d1 & 0x7F)].push_back(notes.size() - 1);
    }
    else if (type == 0x80 || type == 0x90)
    {
      auto& o = open[(ch << 7) | (e.d1 & 0x7F)];
      if (o.empty()) continue;
      Note& n = notes[o.front()];
      n.end = e.tick;
      if (type == 0x80) n.rel = e.d2 & 0x7F;
      o.erase(o.begin());
    }
    else if (type == 0xB0) ccs[{type, ch, e.d1 & 0x7F}].push_back({e.tick, (e.d2 & 0x7F) / 127.0});
    else if (type == 0xE0) ccs[{type, ch, 0}].push_back({e.tick, (((e.d2 & 0x7F) << 7) | (e.d1 & 0x7F)) / 16383.0});
    else if (type == 0xD0) ccs[{type, ch, 0}].push_back({e.tick, (e.d1 & 0x7F) / 127.0});
    else if (type == 0xA0) ccs[{type, ch, e.d1 & 0x7F}].push_back({e.tick, (e.d2 & 0x7F) / 127.0});
    else if (type == 0xC0) ccs[{type, ch, 0}].push_back({e.tick, (e.d1 & 0x7F) / 127.0});
  }

  xml_node parent = clip;
  int parts = (notes.empty() ? 0 : 1) + (int)ccs.size();
  if (parts > 1)
  {
    parent = clip.append_child("Lanes");
    parent.append_attribute("id") = NewId().c_str();
  }
  if (!notes.empty() || parts == 0)
  {
    xml_node nn = parent.append_child("Notes");
    nn.append_attribute("id") = NewId().c_str();
    for (const Note& n : notes)
    {
      long long end = n.end >= 0 ? n.end : last;
      xml_node x = nn.append_child("Note");
      x.append_attribute("time") = Fmt((double)n.start / tpq).c_str();
      x.append_attribute("duration") = Fmt((double)std::max(0LL, end - n.start) / tpq).c_str();
      x.append_attribute("channel") = n.ch;
      x.append_attribute("key") = n.key;
      x.append_attribute("vel") = Fmt(n.vel / 127.0).c_str();
      x.append_attribute("rel") = Fmt(n.rel / 127.0).c_str();
    }
  }
  for (auto& cc : ccs)
  {
    xml_node p = parent.append_child("Points");
    p.append_attribute("id") = NewId().c_str();
    p.append_attribute("unit") = "normalized";
    xml_node target = p.append_child("Target");
    int type = cc.first.status;
    target.append_attribute("expression") = type == 0xB0   ? "channelController"
                                            : type == 0xE0 ? "pitchBend"
                                            : type == 0xD0 ? "channelPressure"
                                            : type == 0xA0 ? "polyPressure"
                                                           : "programChange";
    target.append_attribute("channel") = cc.first.ch;
    if (type == 0xB0) target.append_attribute("controller") = cc.first.num;
    if (type == 0xA0) target.append_attribute("key") = cc.first.num;
    for (auto& v : cc.second)
    {
      xml_node pt = p.append_child("RealPoint");
      pt.append_attribute("time") = Fmt((double)v.first / tpq).c_str();
      pt.append_attribute("value") = Fmt(v.second).c_str();
      pt.append_attribute("interpolation") = "hold";
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Project level

void Exporter::ExportMarkers(xml_node arrangement)
{
  std::vector<std::pair<double, std::pair<std::string, int>>> markers;
  std::set<int> openRegions;
  bool regions = false;
  for (const RppNode* m : root_.Children("MARKER"))
  {
    int idx = m->Int(1), flags = m->Int(4);
    if (flags & 1)
    {
      regions = true;
      if (openRegions.count(idx) && m->Token(3).empty())
      {
        openRegions.erase(idx);
        continue;  // region end
      }
      openRegions.insert(idx);
    }
    markers.push_back({m->Num(2), {m->Token(3), m->Int(5)}});
  }
  if (markers.empty()) return;
  if (regions) Warn("Regions were exported as markers at their start (DAWproject has no regions)");
  std::stable_sort(markers.begin(), markers.end(), [](auto& a, auto& b) { return a.first < b.first; });
  xml_node mk = arrangement.append_child("Markers");
  mk.append_attribute("id") = NewId().c_str();
  for (auto& m : markers)
  {
    xml_node x = mk.append_child("Marker");
    x.append_attribute("time") = Fmt(Beats(m.first)).c_str();
    x.append_attribute("name") = m.second.first.c_str();
    std::string color = HexColor(m.second.second);
    if (!color.empty()) x.append_attribute("color") = color.c_str();
  }
}

void Exporter::ExportTempoAutomation(xml_node arrangement)
{
  if (tempoPoints_.size() > 1 || (tempoPoints_.size() == 1 && tempoPoints_[0].sec > kEps))
  {
    xml_node ta = arrangement.append_child("TempoAutomation");
    ta.append_attribute("id") = NewId().c_str();
    ta.append_attribute("unit") = "bpm";
    ta.append_child("Target").append_attribute("parameter") = tempoId_.c_str();
    for (const EnvPoint& p : tempoPoints_)
    {
      xml_node pt = ta.append_child("RealPoint");
      pt.append_attribute("time") = Fmt(Beats(p.sec)).c_str();
      pt.append_attribute("value") = Fmt(p.value).c_str();
      pt.append_attribute("interpolation") = p.shape == 0 ? "linear" : "hold";
    }
  }
  if (!signatures_.empty())
  {
    xml_node sa = arrangement.append_child("TimeSignatureAutomation");
    sa.append_attribute("id") = NewId().c_str();
    sa.append_child("Target").append_attribute("parameter") = signatureId_.c_str();
    if (signatures_[0].first > kEps)
    {
      xml_node pt = sa.append_child("TimeSignaturePoint");
      pt.append_attribute("time") = "0";
      pt.append_attribute("numerator") = num_;
      pt.append_attribute("denominator") = den_;
    }
    for (auto& s : signatures_)
    {
      xml_node pt = sa.append_child("TimeSignaturePoint");
      pt.append_attribute("time") = Fmt(Beats(s.first)).c_str();
      pt.append_attribute("numerator") = s.second & 0xFFFF;
      pt.append_attribute("denominator") = (s.second >> 16) & 0xFFFF;
    }
  }
}

void Exporter::WriteMetadata(pugi::xml_document& meta)
{
  xml_node m = meta.append_child("MetaData");
  std::map<std::string, std::string> tags;
  if (const RppNode* rm = root_.Child("RENDER_METADATA"))
    for (const RppNode* tag : rm->Children("TAG"))
    {
      std::string key = tag->Token(1);
      size_t colon = key.find(':');
      tags[colon == std::string::npos ? key : key.substr(colon + 1)] = tag->Token(2);
    }
  std::string comment;
  if (const RppNode* notes = root_.Child("NOTES"))
    for (const RppNode& l : notes->children)
      comment += (comment.empty() ? "" : "\n") + (l.line.empty() ? std::string() : l.line.substr(1));
  // schema order
  static const char* fields[][2] = {{"Title", "TIT2"},     {"Artist", "TPE1"},   {"Album", "TALB"},
                                    {"OriginalArtist", "TOPE"}, {"Composer", "TCOM"}, {"Songwriter", "TEXT"},
                                    {"Producer", ""},       {"Arranger", ""},     {"Year", "TYER"},
                                    {"Genre", "TCON"},      {"Copyright", "TCOP"}, {"Website", "WXXX"},
                                    {"Comment", "COMM"}};
  for (auto& f : fields)
  {
    std::string v = *f[1] && tags.count(f[1]) ? tags[f[1]] : std::string();
    if (v.empty() && strcmp(f[0], "Year") == 0 && tags.count("TDRC")) v = tags["TDRC"];
    if (v.empty() && strcmp(f[0], "Artist") == 0)
      if (const RppNode* a = root_.Child("AUTHOR")) v = a->Token(1);
    if (v.empty() && strcmp(f[0], "Comment") == 0) v = comment;
    if (!v.empty()) m.append_child(f[0]).text() = v.c_str();
  }
}

bool Exporter::WriteZip(const std::string& outPath, const std::string& projectXml, const std::string& metaXml,
                        std::string& error)
{
  std::string tmp = outPath + ".part";
  FILE* f = OpenFile(tmp, "wb");
  if (!f)
  {
    error = "Cannot write " + outPath;
    return false;
  }
  mz_zip_archive zip{};
  bool ok = mz_zip_writer_init_cfile(&zip, f, 0) != 0;
  ok = ok && mz_zip_writer_add_mem(&zip, "project.xml", projectXml.data(), projectXml.size(), MZ_DEFAULT_COMPRESSION);
  ok = ok && mz_zip_writer_add_mem(&zip, "metadata.xml", metaXml.data(), metaXml.size(), MZ_DEFAULT_COMPRESSION);
  for (const ZipEntry& e : entries_)
  {
    if (!ok) break;
    if (e.sourcePath.empty())
    {
      ok = mz_zip_writer_add_mem(&zip, e.name.c_str(), e.bytes.data(), e.bytes.size(), MZ_DEFAULT_COMPRESSION) != 0;
      continue;
    }
    FILE* src = OpenFile(e.sourcePath, "rb");
    if (!src)
    {
      error = "Cannot read " + e.sourcePath;
      ok = false;
      break;
    }
    // audio hardly compresses; storing it keeps exporting fast
    ok = mz_zip_writer_add_cfile(&zip, e.name.c_str(), src, (mz_uint64)FileSize(e.sourcePath), nullptr, nullptr, 0,
                                 MZ_NO_COMPRESSION, nullptr, 0, nullptr, 0) != 0;
    fclose(src);
  }
  ok = ok && mz_zip_writer_finalize_archive(&zip);
  mz_zip_writer_end(&zip);
  ok = fclose(f) == 0 && ok;
  if (!ok)
  {
    if (error.empty()) error = "Writing the DAWproject file failed (disk full?)";
    RemoveFile(tmp);
    return false;
  }
  if (!RenameFile(tmp, outPath))
  {
    error = "Cannot replace " + outPath;
    RemoveFile(tmp);
    return false;
  }
  return true;
}

bool Exporter::Run(const std::string& text, const std::string& outPath, std::string& error)
{
  if (!ParseRpp(text, root_, error)) return false;
  if (root_.Name() != "REAPER_PROJECT")
  {
    error = "Not a REAPER project";
    return false;
  }
  CollectPools(root_);
  ReadTempo();

  std::vector<const RppNode*> trackNodes = root_.Children("TRACK");
  for (size_t i = 0; i < trackNodes.size(); i++)
  {
    TrackOut t;
    t.node = trackNodes[i];
    t.index = (int)i;
    tracks_.push_back(t);
  }
  for (TrackOut& t : tracks_) ScanTrack(t);

  xml_node project = doc_.append_child("Project");
  project.append_attribute("version") = "1.0";
  xml_node app = project.append_child("Application");
  app.append_attribute("name") = "REAPER";
  std::string version = root_.Token(2);
  version = version.substr(0, version.find('/'));
  app.append_attribute("version") = version.empty() ? "unknown" : version.c_str();

  xml_node transport = project.append_child("Transport");
  xml_node tempo = transport.append_child("Tempo");
  tempo.append_attribute("unit") = "bpm";
  tempo.append_attribute("min") = "20";
  tempo.append_attribute("max") = "960";
  tempo.append_attribute("value") = Fmt(bpm_).c_str();
  tempoId_ = NewId();
  tempo.append_attribute("id") = tempoId_.c_str();
  xml_node sig = transport.append_child("TimeSignature");
  sig.append_attribute("numerator") = num_;
  sig.append_attribute("denominator") = den_;
  signatureId_ = NewId();
  sig.append_attribute("id") = signatureId_.c_str();

  // The arrangement lanes exist before the structure is filled: FX automation is found there.
  xml_node structure = project.append_child("Structure");
  xml_node arrangement = project.append_child("Arrangement");
  arrangement.append_attribute("id") = NewId().c_str();
  rootLanes_ = arrangement.append_child("Lanes");
  rootLanes_.append_attribute("timeUnit") = "beats";
  rootLanes_.append_attribute("id") = NewId().c_str();
  BuildStructure(structure);

  BuildRouting();
  ExportTrackEnvelopes(master_);
  for (TrackOut& t : tracks_)
  {
    for (const RppNode* item : t.node->Children("ITEM")) ExportItem(t, *item);
    ExportTrackEnvelopes(t);
  }
  ExportMarkers(arrangement);
  ExportTempoAutomation(arrangement);
  if (root_.Child("TEMPOENVEX") && tempoPoints_.size() > 1)
    for (const EnvPoint& p : tempoPoints_)
      if (p.shape > 1) Warn("Curved tempo changes were exported as linear");

  pugi::xml_document meta;
  WriteMetadata(meta);

  struct StringWriter : pugi::xml_writer
  {
    std::string s;
    void write(const void* data, size_t size) override { s.append((const char*)data, size); }
  };
  StringWriter pw, mw;
  pugi::xml_node decl = doc_.prepend_child(pugi::node_declaration);
  decl.append_attribute("version") = "1.0";
  decl.append_attribute("encoding") = "UTF-8";
  doc_.save(pw, "  ", pugi::format_default, pugi::encoding_utf8);
  pugi::xml_node mdecl = meta.prepend_child(pugi::node_declaration);
  mdecl.append_attribute("version") = "1.0";
  mdecl.append_attribute("encoding") = "UTF-8";
  meta.save(mw, "  ", pugi::format_default, pugi::encoding_utf8);

  if (!WriteZip(outPath, pw.s, mw.s, error)) return false;

  report_.tracks = (int)tracks_.size();
  for (auto& w : warnings_)
    report_.warnings.push_back(w.second > 1 ? w.first + " (" + std::to_string(w.second) + "x)" : w.first);
  return true;
}

}  // namespace

bool ExportDawProject(const std::string& rppText, const std::string& outPath, const ExportOptions& options,
                      ExportReport& report, std::string& error)
{
  Exporter e(options, report);
  return e.Run(rppText, outPath, error);
}

}  // namespace dawp
