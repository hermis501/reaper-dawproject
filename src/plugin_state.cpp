#include "plugin_state.h"

#include <cstring>

#include "rpp_sink.h"
#include "util.h"

namespace dawp {

// REAPER stores plug-in state as base64 blocks:
//   1. header: id, 0xFEED5EEE/F, input/output pin masks, data size, flags
//   2. data:   the plug-in's state
//   3. footer: program name, preset name
// Layout verified against REAPER 7 project files.
static const uint32_t kOpaque = 0xFEED5EEE;
static const uint32_t kRegular = 0xFEED5EEF;

static void PutLE32(std::vector<uint8_t>& v, uint32_t x)
{
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
static void PutLE64(std::vector<uint8_t>& v, uint64_t x)
{
  for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
static uint32_t GetBE32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t GetLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t GetLE64(const uint8_t* p) { return GetLE32(p) | ((uint64_t)GetLE32(p + 4) << 32); }

static void AddBase64Lines(std::vector<std::string>& lines, const std::vector<uint8_t>& data)
{
  std::string b = Base64Encode(data.data(), data.size());
  for (size_t i = 0; i < b.size(); i += 128) lines.push_back(b.substr(i, 128));
}

static std::vector<uint8_t> Header(uint32_t id, uint32_t type, uint32_t dataSize, bool opaque)
{
  std::vector<uint8_t> h;
  PutLE32(h, id);
  PutLE32(h, type);
  PutLE32(h, 2);  // inputs, with one bit mask per pin
  PutLE64(h, 1);
  PutLE64(h, 2);
  PutLE32(h, 2);  // outputs
  PutLE64(h, 1);
  PutLE64(h, 2);
  PutLE32(h, dataSize);
  PutLE32(h, opaque ? 1 : 0);
  PutLE32(h, 0);
  return h;
}

static std::vector<uint8_t> Footer()
{
  // empty program name, empty preset name, trailing flags
  return std::vector<uint8_t>{0, 0, 0x10, 0, 0, 0};
}

static std::string DisplayName(const PluginInfo& info, const char* prefix)
{
  std::string s = prefix;
  if (info.instrument) s += "i";
  s += ": " + info.name;
  if (!info.vendor.empty()) s += " (" + info.vendor + ")";
  return s;
}

static int HexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool ParseHex16(const std::string& hex, uint8_t out[16])
{
  std::string h;
  for (char c : hex)
    if (HexVal(c) >= 0) h += c;
  if (h.size() != 32) return false;
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)(HexVal(h[2 * i]) * 16 + HexVal(h[2 * i + 1]));
  return true;
}

uint32_t ReaperVst3Hash(const std::string& hexClassId)
{
  uint8_t raw[16];
  if (!ParseHex16(hexClassId, raw)) return 0;
  // COM GUID layout: first three groups little endian
  uint8_t g[16] = {raw[3], raw[2], raw[1], raw[0], raw[5], raw[4], raw[7], raw[6]};
  memcpy(g + 8, raw + 8, 8);
  uint32_t h = 0x811c9dc5;
  for (uint8_t b : g)
  {
    h ^= b;
    h *= 0x01000193;
  }
  return h & 0x7fffffff;
}

// .vstpreset: "VST3", version, 32-char class id, chunk list offset; list "List" with Comp/Cont.
static bool ParseVstPreset(const std::vector<uint8_t>& p, std::vector<uint8_t>& comp, std::vector<uint8_t>& cont,
                           std::string& classId, std::string& error)
{
  if (p.size() < 48 || memcmp(p.data(), "VST3", 4) != 0)
  {
    error = "not a .vstpreset file";
    return false;
  }
  classId.assign((const char*)p.data() + 8, 32);
  uint64_t listOffset = GetLE64(p.data() + 40);
  bool found = false;
  if (listOffset + 8 <= p.size() && memcmp(p.data() + listOffset, "List", 4) == 0)
  {
    uint32_t count = GetLE32(p.data() + listOffset + 4);
    const uint8_t* e = p.data() + listOffset + 8;
    for (uint32_t i = 0; i < count && e + 20 <= p.data() + p.size(); i++, e += 20)
    {
      uint64_t off = GetLE64(e + 4), size = GetLE64(e + 12);
      if (off + size > p.size()) continue;
      if (memcmp(e, "Comp", 4) == 0)
      {
        comp.assign(p.begin() + off, p.begin() + off + size);
        found = true;
      }
      else if (memcmp(e, "Cont", 4) == 0)
        cont.assign(p.begin() + off, p.begin() + off + size);
    }
  }
  if (!found)
  {
    // No chunk list: everything between header and list is the component state.
    uint64_t end = listOffset > 48 && listOffset <= p.size() ? listOffset : p.size();
    comp.assign(p.begin() + 48, p.begin() + end);
  }
  return true;
}

static bool BuildVst3(const PluginInfo& info, const std::vector<uint8_t>& state, std::vector<std::string>& lines,
                      std::string& error)
{
  std::vector<uint8_t> comp, cont;
  std::string classId = info.id;
  bool stateOk = false;
  if (!state.empty())
  {
    std::string presetClass;
    stateOk = ParseVstPreset(state, comp, cont, presetClass, error);
    if (stateOk && classId.empty()) classId = presetClass;
  }
  uint8_t dummy[16];
  if (!ParseHex16(classId, dummy))
  {
    error = "missing or invalid VST3 class id";
    return false;
  }
  for (char& c : classId) c = (char)toupper((unsigned char)c);
  uint32_t hash = ReaperVst3Hash(classId);

  lines.push_back("<VST " + RppQuote(DisplayName(info, "VST3")) + " \"\" 0 \"\" " + std::to_string(hash) + "{" +
                  classId + "} \"\"");
  if (stateOk)
  {
    std::vector<uint8_t> data;
    PutLE32(data, (uint32_t)comp.size());
    PutLE32(data, 1);
    data.insert(data.end(), comp.begin(), comp.end());
    PutLE32(data, (uint32_t)cont.size());
    PutLE32(data, 0);
    data.insert(data.end(), cont.begin(), cont.end());
    AddBase64Lines(lines, Header(hash, kOpaque, (uint32_t)data.size(), true));
    AddBase64Lines(lines, data);
    AddBase64Lines(lines, Footer());
  }
  lines.push_back(">");
  return true;
}

// .fxp / .fxb: "CcnK", size, kind, version, fxID, fxVersion, count, then kind-specific data (big endian).
static bool BuildVst2(const PluginInfo& info, const std::vector<uint8_t>& p, std::vector<std::string>& lines,
                      std::string& error)
{
  uint32_t fxId = (uint32_t)strtoul(info.id.c_str(), nullptr, 10);
  std::vector<uint8_t> data;
  bool opaque = true, ok = true;

  if (!p.empty())
  {
    if (p.size() < 28 || memcmp(p.data(), "CcnK", 4) != 0)
    {
      error = "not an .fxp/.fxb file";
      ok = false;
    }
    else
    {
      std::string kind((const char*)p.data() + 8, 4);
      if (!fxId) fxId = GetBE32(p.data() + 16);
      if (kind == "FPCh" && p.size() >= 60)
      {
        uint32_t size = GetBE32(p.data() + 56);
        if (60 + (size_t)size <= p.size()) data.assign(p.begin() + 60, p.begin() + 60 + size);
        else ok = false;
      }
      else if (kind == "FBCh" && p.size() >= 160)
      {
        uint32_t size = GetBE32(p.data() + 156);
        if (160 + (size_t)size <= p.size()) data.assign(p.begin() + 160, p.begin() + 160 + size);
        else ok = false;
      }
      else if (kind == "FxCk" && p.size() >= 56)
      {
        // Parameter list: REAPER wants little-endian floats after two magic words.
        opaque = false;
        uint32_t n = GetBE32(p.data() + 24);
        PutLE32(data, 0xDEADBEEF);
        PutLE32(data, 0xDEADF00D);
        for (uint32_t i = 0; i < n && 56 + 4 * (size_t)i + 4 <= p.size(); i++) PutLE32(data, GetBE32(p.data() + 56 + 4 * i));
      }
      else
      {
        error = "unsupported VST2 preset kind '" + kind + "'";
        ok = false;
      }
      if (!ok && error.empty()) error = "truncated VST2 preset";
    }
  }

  if (!fxId)
  {
    error = "missing VST2 plug-in id";
    return false;
  }
  if (!ok) data.clear();

  // REAPER's VST2 id: "<numeric id><16 bytes as hex>" where the bytes are "VST" + 4CC + lower-case name.
  std::string fake = "VST";
  for (int i = 3; i >= 0; i--) fake += (char)((fxId >> (8 * i)) & 0xFF);
  fake += ToLower(info.name);
  fake.resize(16, '\0');
  static const char* hex = "0123456789ABCDEF";  // as REAPER writes it
  std::string idHex;
  for (unsigned char c : fake)
  {
    idHex += hex[c >> 4];
    idHex += hex[c & 15];
  }

  lines.push_back("<VST " + RppQuote(DisplayName(info, "VST")) + " \"\" 0 \"\" " + std::to_string(fxId) + "<" +
                  idHex + "> \"\"");
  if (!data.empty())
  {
    AddBase64Lines(lines, Header(fxId, opaque ? kOpaque : kRegular, (uint32_t)data.size(), opaque));
    AddBase64Lines(lines, data);
    AddBase64Lines(lines, Footer());
  }
  lines.push_back(">");
  return true;
}

static bool BuildClap(const PluginInfo& info, const std::vector<uint8_t>& state, std::vector<std::string>& lines,
                      std::string& error)
{
  if (info.id.empty())
  {
    error = "missing CLAP id";
    return false;
  }
  lines.push_back("<CLAP " + RppQuote(DisplayName(info, "CLAP")) + " " + RppQuote(info.id) + " \"\"");
  if (!state.empty())
  {
    lines.push_back("<STATE");
    AddBase64Lines(lines, state);
    lines.push_back(">");
  }
  lines.push_back(">");
  return true;
}

// ---------------------------------------------------------------------------------------------
// REAPER chunk -> DAWproject state file

static void PutBE32(std::vector<uint8_t>& v, uint32_t x)
{
  for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i)));
}

// "VST3i: Name (Vendor)" -> "VST3i", "Name", "Vendor"
static void SplitDisplayName(const std::string& s, std::string& prefix, std::string& name, std::string& vendor)
{
  size_t colon = s.find(": ");
  if (colon != std::string::npos && colon <= 6)
  {
    prefix = s.substr(0, colon);
    name = s.substr(colon + 2);
  }
  else
    name = s;
  if (name.empty() || name.back() != ')') return;
  int depth = 0;
  for (size_t i = name.size(); i-- > 0;)
  {
    if (name[i] == ')') depth++;
    else if (name[i] == '(' && --depth == 0)
    {
      if (i > 0 && name[i - 1] == ' ')
      {
        vendor = name.substr(i + 1, name.size() - i - 2);
        name = name.substr(0, i - 1);
      }
      return;
    }
  }
}

// Decodes REAPER's VST state lines. Each line is valid base64 by itself.
static bool DecodeVstState(const std::vector<std::string>& lines, uint32_t& magic, std::vector<uint8_t>& data)
{
  std::vector<uint8_t> raw;
  for (const std::string& l : lines) Base64Decode(l, raw);
  if (raw.size() < 16) return false;
  magic = GetLE32(raw.data() + 4);
  if (magic != kOpaque && magic != kRegular) return false;
  size_t off = 8;
  uint32_t nIn = GetLE32(raw.data() + off);
  off += 4 + (size_t)nIn * 8;
  if (off + 4 > raw.size()) return false;
  uint32_t nOut = GetLE32(raw.data() + off);
  off += 4 + (size_t)nOut * 8;
  if (off + 12 > raw.size()) return false;
  uint32_t size = GetLE32(raw.data() + off);
  off += 12;  // data size and two flag words
  if (off + size > raw.size()) return false;
  data.assign(raw.begin() + off, raw.begin() + off + size);
  return true;
}

bool ReadReaperPlugin(const std::vector<std::string>& tokens, const std::vector<std::string>& base64,
                      ReaperPluginState& out, std::string& error)
{
  if (tokens.size() < 3)
  {
    error = "incomplete plug-in line";
    return false;
  }
  std::string prefix;
  SplitDisplayName(tokens[1], prefix, out.info.name, out.info.vendor);
  out.info.instrument = !prefix.empty() && prefix.back() == 'i';

  if (tokens[0] == "CLAP")
  {
    out.info.format = PluginFormat::Clap;
    out.info.id = tokens[2];
    out.fileExtension = "clap-preset";
    for (const std::string& l : base64) Base64Decode(l, out.state);
    return !out.info.id.empty();
  }
  if (tokens[0] != "VST" || tokens.size() < 6)
  {
    error = "unsupported plug-in type";
    return false;
  }

  const std::string& id = tokens[5];
  size_t brace = id.find('{'), angle = id.find('<');
  uint32_t magic = 0;
  std::vector<uint8_t> data;
  bool haveState = !base64.empty();
  if (haveState && !DecodeVstState(base64, magic, data))
  {
    error = "unrecognized state data";
    haveState = false;
  }

  if (brace != std::string::npos)
  {
    out.info.format = PluginFormat::Vst3;
    size_t close = id.find('}', brace);
    out.info.id = id.substr(brace + 1, close == std::string::npos ? std::string::npos : close - brace - 1);
    uint8_t check[16];
    if (!ParseHex16(out.info.id, check))
    {
      error = "invalid VST3 class id";
      return false;
    }
    out.fileExtension = "vstpreset";
    if (!haveState) return true;
    // data: [component size][flag][component state][controller size][flag][controller state]
    if (data.size() < 8 || 8 + (size_t)GetLE32(data.data()) > data.size())
    {
      error = "unrecognized VST3 state";
      return true;
    }
    uint32_t compLen = GetLE32(data.data());
    std::vector<uint8_t> comp(data.begin() + 8, data.begin() + 8 + compLen), cont;
    size_t off = 8 + compLen;
    if (off + 8 <= data.size())
    {
      uint32_t contLen = GetLE32(data.data() + off);
      if (off + 8 + contLen <= data.size()) cont.assign(data.begin() + off + 8, data.begin() + off + 8 + contLen);
    }
    std::vector<uint8_t>& p = out.state;
    p.insert(p.end(), {'V', 'S', 'T', '3'});
    PutLE32(p, 1);
    p.insert(p.end(), out.info.id.begin(), out.info.id.end());
    PutLE64(p, 48 + comp.size() + cont.size());
    p.insert(p.end(), comp.begin(), comp.end());
    p.insert(p.end(), cont.begin(), cont.end());
    p.insert(p.end(), {'L', 'i', 's', 't'});
    PutLE32(p, cont.empty() ? 1 : 2);
    p.insert(p.end(), {'C', 'o', 'm', 'p'});
    PutLE64(p, 48);
    PutLE64(p, comp.size());
    if (!cont.empty())
    {
      p.insert(p.end(), {'C', 'o', 'n', 't'});
      PutLE64(p, 48 + comp.size());
      PutLE64(p, cont.size());
    }
    return true;
  }

  if (angle == std::string::npos && id.find_first_not_of("0123456789") != std::string::npos)
  {
    error = "unrecognized plug-in id";
    return false;
  }
  out.info.format = PluginFormat::Vst2;
  uint32_t fxId = (uint32_t)strtoul(id.c_str(), nullptr, 10);
  out.info.id = std::to_string(fxId);
  out.fileExtension = "fxp";
  if (!haveState) return true;

  // .fxp: opaque chunks as FPCh, parameter lists as FxCk (big endian)
  std::vector<uint8_t> body;
  bool params = magic == kRegular;
  size_t first = 0;
  if (params && data.size() >= 8 && GetLE32(data.data()) == 0xDEADBEEF && GetLE32(data.data() + 4) == 0xDEADF00D) first = 8;
  uint32_t count = params ? (uint32_t)((data.size() - first) / 4) : 1;
  const char* kind = params ? "FxCk" : "FPCh";
  body.insert(body.end(), kind, kind + 4);
  PutBE32(body, 1);  // format version
  PutBE32(body, fxId);
  PutBE32(body, 1);  // plug-in version
  PutBE32(body, count);
  body.resize(body.size() + 28, 0);  // program name
  if (params)
    for (uint32_t i = 0; i < count; i++) PutBE32(body, GetLE32(data.data() + first + 4 * i));
  else
  {
    PutBE32(body, (uint32_t)data.size());
    body.insert(body.end(), data.begin(), data.end());
  }
  std::vector<uint8_t>& p = out.state;
  p.insert(p.end(), {'C', 'c', 'n', 'K'});
  PutBE32(p, (uint32_t)body.size());
  p.insert(p.end(), body.begin(), body.end());
  return true;
}

bool BuildPluginChunk(const PluginInfo& info, const std::vector<uint8_t>& state, std::vector<std::string>& lines,
                      std::string& error)
{
  switch (info.format)
  {
    case PluginFormat::Vst3: return BuildVst3(info, state, lines, error);
    case PluginFormat::Vst2: return BuildVst2(info, state, lines, error);
    case PluginFormat::Clap: return BuildClap(info, state, lines, error);
  }
  return false;
}

}  // namespace dawp
