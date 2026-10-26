// reaper_dawproject: opens .dawproject files via File > Open project (and drag & drop) like native
// projects, adds "Export project to DAWproject..." to the File menu (also an action) and installs
// the JSFX that stand in for DAWproject built-in devices.

#include <exception>
#include <string>
#include <vector>

#include "builtin_fx.h"
#include "converter.h"
#include "exporter.h"
#include "reaper_ext.h"
#include "util.h"

using namespace rext;

namespace {

// ---------------------------------------------------------------------------------------------
// Bundled JSFX

void InstallJsfx()
{
  if (!GetResourcePath) return;
  std::string dir = dawp::JoinPath(dawp::JoinPath(GetResourcePath(), "Effects"), "DAWproject");
  if (!dawp::CreateDirectories(dir)) return;
  for (const dawp::JsfxFile& f : dawp::BundledJsfx())
  {
    std::string path = dawp::JoinPath(dir, f.name);
    std::vector<uint8_t> existing;
    std::string text = f.text;
    if (dawp::ReadFileBytes(path, existing) && std::string(existing.begin(), existing.end()) == text) continue;
    dawp::WriteFileBytes(path, text.data(), text.size());
  }
}

// ---------------------------------------------------------------------------------------------
// Import

// Work that needs the loaded project (VST3/CLAP parameter envelopes, VCA groups) runs once from
// the timer after REAPER finished opening it.
struct PostLoad
{
  std::string file;
  int tracks = 0;
  std::vector<dawp::PostLoadEnvelope> envelopes;
  std::vector<dawp::PostLoadGroup> groups;
} g_postLoad;

bool IdentMatches(const std::string& reaperIdent, const std::string& id)
{
  if (reaperIdent == id) return true;
  // tolerate decorated identifiers such as "index:id"
  size_t colon = reaperIdent.rfind(':');
  return colon != std::string::npos && reaperIdent.substr(colon + 1) == id;
}

void ApplyPostLoad()
{
  g_rec->Register("-timer", (void*)ApplyPostLoad);
  PostLoad work = std::move(g_postLoad);
  g_postLoad = PostLoad();

  ReaProject* proj = EnumProjects(-1, nullptr, 0);
  if (!proj || CountTracks(proj) != work.tracks)
  {
    Report("DAWproject import: the project changed before plug-in automation and VCA groups could be applied.\n");
    return;
  }
  std::vector<std::string> problems;
  int applied = 0;
  for (const dawp::PostLoadEnvelope& e : work.envelopes)
  {
    MediaTrack* tr = e.track < 0 ? GetMasterTrack(proj) : GetTrack(proj, e.track);
    if (!tr || e.fx >= TrackFX_GetCount(tr))
    {
      problems.push_back("automation for '" + e.fxName + "': plug-in not found");
      continue;
    }
    int found = -1;
    int n = TrackFX_GetNumParams(tr, e.fx);
    char buf[256];
    for (int p = 0; p < n && found < 0; p++)
      if (TrackFX_GetParamIdent(tr, e.fx, p, buf, sizeof(buf)) && IdentMatches(buf, e.paramIdent)) found = p;
    if (found < 0)
    {
      problems.push_back("automation for '" + e.fxName + "': parameter id " + e.paramIdent +
                         " not found (plug-in not installed or not loaded?)");
      continue;
    }
    TrackEnvelope* env = GetFXEnvelope(tr, e.fx, found, true);
    if (!env)
    {
      problems.push_back("automation for '" + e.fxName + "': envelope could not be created");
      continue;
    }
    DeleteEnvelopePointRange(env, -1e9, 1e12);
    bool noSort = true;
    for (const auto& p : e.points) InsertEnvelopePoint(env, p.sec, p.value, p.shape, 0, false, &noSort);
    Envelope_SortPoints(env);
    applied++;
  }
  for (const dawp::PostLoadGroup& g : work.groups)
  {
    MediaTrack* tr = GetTrack(proj, g.track);
    if (!tr) continue;
    unsigned int bit = 1u << (g.group - 1);
    GetSetTrackGroupMembership(tr, g.flag.c_str(), bit, bit);
    std::string key = "TRACK_GROUP_NAME:" + std::to_string(g.group);
    std::string name = g.groupName;
    GetSetProjectInfo_String(proj, key.c_str(), &name[0], true);
  }
  if (UpdateArrange) UpdateArrange();

  if (!problems.empty())
  {
    std::string msg = "DAWproject import: " + dawp::FileName(work.file) + "\n" + std::to_string(applied) +
                      " plug-in automation envelope(s) applied. Not applied:\n";
    for (const std::string& p : problems) msg += "- " + p + "\n";
    Report(msg);
  }
}

bool WantProjectFile(const char* fn)
{
  return fn && dawp::LowerExtension(fn) == "dawproject";
}

const char* EnumFileExtensions(int i, char** descptr)
{
  if (i == 0)
  {
    if (descptr) *descptr = (char*)"DAWproject files";
    return "DAWPROJECT";
  }
  if (descptr) *descptr = nullptr;
  return nullptr;
}

bool HavePostLoadApi()
{
  return EnumProjects && GetTrack && GetMasterTrack && CountTracks && TrackFX_GetCount && TrackFX_GetNumParams &&
         TrackFX_GetParamIdent && GetFXEnvelope && InsertEnvelopePoint && DeleteEnvelopePointRange &&
         Envelope_SortPoints && GetSetTrackGroupMembership && GetSetProjectInfo_String;
}

int LoadProject(const char* fn, ProjectStateContext* ctx)
{
  dawp::ConvertOptions options;
  options.postLoadFixups = HavePostLoadApi();
  dawp::ConvertReport report;
  std::string error;
  ContextSink sink(ctx);
  bool ok = false;
  try
  {
    ok = dawp::ConvertDawProject(fn, sink, options, report, error);
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "unexpected error";
  }

  if (!ok)
  {
    Report("DAWproject import failed: " + dawp::FileName(fn) + "\n" + error + "\n");
    return -1;
  }
  if (!report.envelopes.empty() || !report.groups.empty())
  {
    g_postLoad.file = fn;
    g_postLoad.tracks = report.tracks;
    g_postLoad.envelopes = std::move(report.envelopes);
    g_postLoad.groups = std::move(report.groups);
    g_rec->Register("timer", (void*)ApplyPostLoad);
  }
  if (!report.warnings.empty())
  {
    std::string msg = "DAWproject import: " + dawp::FileName(fn) + "\n";
    if (!report.application.empty()) msg += "Exported by " + report.application + ". ";
    msg += std::to_string(report.tracks) + " tracks, " + std::to_string(report.items) + " items, " +
           std::to_string(report.plugins) + " plug-ins.\n";
    if (!report.mediaDir.empty()) msg += "Embedded media extracted to: " + report.mediaDir + "\n";
    msg += "Not converted:\n";
    for (const std::string& w : report.warnings) msg += "- " + w + "\n";
    Report(msg);
  }
  return 0;
}

project_import_register_t g_import = {WantProjectFile, EnumFileExtensions, LoadProject};

// ---------------------------------------------------------------------------------------------
// Export

void ExportProject()
{
  ReaProject* proj;
  std::string projectFile, target, rpp;
  if (!PrepareExport("Export project to DAWproject", "dawproject", "DAWproject files (*.dawproject)|*.dawproject", proj,
                     projectFile, target, rpp))
    return;

  dawp::ExportOptions options;
  options.mediaDirs.push_back(dawp::TempDirectory());  // REAPER may write paths relative to the copy
  if (!projectFile.empty()) options.mediaDirs.push_back(dawp::DirName(projectFile));
  options.probeMedia = ProbeWithReaper;
  options.paramIdent = [proj](int track, int fx, int param) {
    MediaTrack* tr = track < 0 ? GetMasterTrack(proj) : GetTrack(proj, track);
    char buf[256];
    if (!tr || !TrackFX_GetParamIdent(tr, fx, param, buf, sizeof(buf))) return std::string();
    std::string ident = buf;
    size_t colon = ident.rfind(':');
    return colon == std::string::npos ? ident : ident.substr(colon + 1);
  };
  dawp::ExportReport report;
  std::string error;
  bool ok = false;
  try
  {
    ok = dawp::ExportDawProject(rpp, target, options, report, error);
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  if (!ok)
  {
    Report("DAWproject export failed: " + error + "\n");
    return;
  }
  std::string msg = "DAWproject export: saved " + target + "\n" + std::to_string(report.tracks) + " tracks, " +
                    std::to_string(report.clips) + " clips, " + std::to_string(report.plugins) + " plug-ins, " +
                    std::to_string(report.mediaFiles) + " media files embedded.\n";
  if (!report.warnings.empty())
  {
    msg += "Not exported:\n";
    for (const std::string& w : report.warnings) msg += "- " + w + "\n";
  }
  Report(msg);
}

std::vector<ExportCommand> g_commands = {
    {"DAWPROJECT_EXPORT", "DAWproject: Export project to DAWproject...", "Export project to DAWproject...", ExportProject},
};

}  // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t* rec)
{
  (void)hInstance;
  if (!rec) return 0;  // unloading
  if (!Init(rec)) return 0;
  if (!rec->Register("projectimport", &g_import)) return 0;
  InstallJsfx();
  RegisterExportCommands(g_commands);
  return 1;
}
