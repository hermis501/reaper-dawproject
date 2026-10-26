// Converts a .dawproject file into REAPER project (RPP) lines.
#pragma once

#include <string>
#include <vector>

#include "rpp_sink.h"

namespace dawp {

struct ConvertOptions
{
  // Folder for audio/video embedded in the container. Empty: "<project name>-media" next to
  // the .dawproject file, falling back to the temp folder if that is not writable.
  std::string mediaDir;
  // The caller (the REAPER extension) applies ConvertReport::envelopes and ::groups after REAPER
  // has loaded the project. Without it these are reported as not converted.
  bool postLoadFixups = false;
};

// Automation of a VST3/CLAP parameter. REAPER addresses parameters by index, which is only
// known once the plug-in is loaded; 'paramIdent' is the plug-in's own parameter id.
struct PostLoadEnvelope
{
  struct Point
  {
    double sec, value;
    int shape;
  };
  int track;  // -1 = master
  int fx;
  std::string paramIdent;
  std::string fxName;
  std::vector<Point> points;  // normalized values
};

// Track group membership, e.g. VCA lead/follow ("VOLUME_VCA_LEAD", "VOLUME_VCA_FOLLOW").
struct PostLoadGroup
{
  int track;
  std::string flag;
  int group;  // 1..32
  std::string groupName;
};

struct ConvertReport
{
  std::vector<std::string> warnings;  // things that could not be converted, deduplicated
  std::string mediaDir;               // where embedded media was extracted (if any)
  std::string application;            // exporting application, e.g. "Bitwig Studio 5.0"
  int tracks = 0;
  int items = 0;
  int plugins = 0;
  bool usesBundledJsfx = false;  // built-in devices became the bundled JSFX
  std::vector<PostLoadEnvelope> envelopes;
  std::vector<PostLoadGroup> groups;
};

bool ConvertDawProject(const std::string& path, RppSink& sink, const ConvertOptions& options, ConvertReport& report,
                       std::string& error);

// Converts DAWproject-style project XML that another importer built in memory (all media
// external). It may use these extensions, which are not part of DAWproject:
//   <Project xTimeOffset="sec">, <Project><xMetaData>...</xMetaData> (as metadata.xml),
//   <Clip xGain="linear" xFadeInShape="n" xFadeOutShape="n"> (REAPER fade shapes),
//   <Clip><xGainEnvelope><Point time="sec from clip start" value="linear" shape="0|1"/>,
//   <Audio xChannel="n"> (play only channel n, 0-based, of a multichannel file),
//   <Marker xDuration="sec"> (a region).
bool ConvertProjectXml(const std::string& xml, const std::string& sourcePath, RppSink& sink,
                       const ConvertOptions& options, ConvertReport& report, std::string& error);

}  // namespace dawp
