// Read access to a .dawproject file (a ZIP container with project.xml, metadata.xml and media).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dawp {

class DawProjectContainer
{
public:
  DawProjectContainer();
  ~DawProjectContainer();
  DawProjectContainer(const DawProjectContainer&) = delete;
  DawProjectContainer& operator=(const DawProjectContainer&) = delete;

  bool Open(const std::string& path, std::string& error);
  const std::string& Path() const { return path_; }

  bool HasEntry(const std::string& name) const;
  bool ReadEntry(const std::string& name, std::vector<uint8_t>& out) const;
  bool ExtractEntry(const std::string& name, const std::string& destPath) const;
  int64_t EntrySize(const std::string& name) const;  // -1 if missing

  // Normalizes a path from the XML to the form used inside the ZIP.
  static std::string EntryName(const std::string& xmlPath);

private:
  int Locate(const std::string& name) const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string path_;
};

}  // namespace dawp
