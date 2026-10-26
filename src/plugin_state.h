// Builds REAPER FX chunks (<VST ...>, <CLAP ...>) from DAWproject plug-in state files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dawp {

enum class PluginFormat { Vst2, Vst3, Clap };

struct PluginInfo
{
  PluginFormat format = PluginFormat::Vst3;
  std::string name;    // deviceName
  std::string vendor;  // deviceVendor
  std::string id;      // deviceID: VST2 numeric id, VST3 32-hex class id, CLAP id
  bool instrument = false;
};

// Appends the plug-in block lines (from "<VST ..." to the closing ">") to 'lines'.
// 'state' may be empty when the DAWproject has no state file; REAPER then loads defaults.
// Returns false (with 'error') if the plug-in cannot be identified and nothing was written.
// If only the state is unreadable, the block is written without it and 'error' is set.
bool BuildPluginChunk(const PluginInfo& info, const std::vector<uint8_t>& state, std::vector<std::string>& lines,
                      std::string& error);

// A plug-in read from a REAPER FX chain, with its state converted to a DAWproject state file.
struct ReaperPluginState
{
  PluginInfo info;
  std::vector<uint8_t> state;  // state file contents, empty if the chunk has no state
  std::string fileExtension;   // "vstpreset", "fxp" or "clap-preset"
};

// 'tokens': header tokens of a <VST ...> or <CLAP ...> block. 'base64': the state lines (the VST
// block's lines, or the lines of a CLAP block's <STATE> child). Returns false if the plug-in
// cannot be identified; if only the state is unreadable, it returns true with 'error' set.
bool ReadReaperPlugin(const std::vector<std::string>& tokens, const std::vector<std::string>& base64,
                      ReaperPluginState& out, std::string& error);

// REAPER's numeric id for a VST3 class id (FNV-1a over the GUID in COM byte order).
uint32_t ReaperVst3Hash(const std::string& hexClassId);

}  // namespace dawp
