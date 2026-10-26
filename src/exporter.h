// Converts a REAPER project (RPP text) into a .dawproject file.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "media_info.h"

namespace dawp {

struct ExportOptions
{
  // Copy audio files into the .dawproject (portable). Otherwise they are referenced by absolute path.
  bool embedMedia = true;
  // Folders that relative media paths in the project are relative to, tried in order (normally
  // just the .rpp's folder).
  std::vector<std::string> mediaDirs;
  // Hooks the REAPER extension provides; both may be empty.
  // Audio file information for formats the built-in reader does not know (MP3, OGG, ...).
  std::function<bool(const std::string& file, MediaInfo& info)> probeMedia;
  // The VST3/CLAP parameter id of FX parameter 'param' (REAPER index) of FX 'fx' on track 'track'
  // (0-based, -1 = master), as a decimal string. Without it, VST3/CLAP automation is skipped.
  std::function<std::string(int track, int fx, int param)> paramIdent;
};

struct ExportReport
{
  std::vector<std::string> warnings;  // what could not be exported, deduplicated
  int tracks = 0;
  int clips = 0;
  int plugins = 0;
  int mediaFiles = 0;
};

bool ExportDawProject(const std::string& rppText, const std::string& outPath, const ExportOptions& options,
                      ExportReport& report, std::string& error);

}  // namespace dawp
