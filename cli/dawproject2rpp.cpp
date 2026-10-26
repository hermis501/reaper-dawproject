// Command-line converter using the same code as the REAPER extension.
//   dawproject2rpp <input.dawproject> [output.rpp | -] [--media <folder>]

#include <cstdio>
#include <string>
#include <vector>

#include "converter.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int Run(const std::vector<std::string>& args)
{
  std::string input, output, media;
  for (size_t i = 1; i < args.size(); i++)
  {
    if (args[i] == "--media" && i + 1 < args.size()) media = args[++i];
    else if (input.empty()) input = args[i];
    else if (output.empty()) output = args[i];
  }
  if (input.empty())
  {
    fprintf(stderr, "usage: dawproject2rpp <input.dawproject> [output.rpp | -] [--media <folder>]\n");
    return 2;
  }
  if (output.empty()) output = dawp::JoinPath(dawp::DirName(input), dawp::FileStem(input) + ".rpp");

  FILE* f = output == "-" ? stdout : dawp::OpenFile(output, "wb");
  if (!f)
  {
    fprintf(stderr, "cannot write %s\n", output.c_str());
    return 1;
  }
  dawp::FileRppSink sink(f);
  dawp::ConvertOptions options;
  options.mediaDir = media;
  dawp::ConvertReport report;
  std::string error;
  bool ok = dawp::ConvertDawProject(input, sink, options, report, error);
  if (f != stdout) fclose(f);
  if (!ok)
  {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  fprintf(stderr, "%s: %d tracks, %d items, %d plug-ins%s%s\n", report.application.c_str(), report.tracks,
          report.items, report.plugins, report.mediaDir.empty() ? "" : ", media in ", report.mediaDir.c_str());
  for (const std::string& w : report.warnings) fprintf(stderr, "warning: %s\n", w.c_str());
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
    args.push_back(s);
  }
  return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv, argv + argc)); }
#endif
