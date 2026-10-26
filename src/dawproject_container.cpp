#include "dawproject_container.h"

#include "miniz.h"
#include "util.h"

namespace dawp {

struct DawProjectContainer::Impl
{
  mz_zip_archive zip{};
  FILE* file = nullptr;
  bool open = false;
};

DawProjectContainer::DawProjectContainer() : impl_(new Impl) {}

DawProjectContainer::~DawProjectContainer()
{
  if (impl_->open) mz_zip_reader_end(&impl_->zip);
  if (impl_->file) fclose(impl_->file);
}

bool DawProjectContainer::Open(const std::string& path, std::string& error)
{
  path_ = path;
  int64_t size = FileSize(path);
  if (size < 0)
  {
    error = "File not found: " + path;
    return false;
  }
  impl_->file = OpenFile(path, "rb");
  if (!impl_->file)
  {
    error = "Cannot open file: " + path;
    return false;
  }
  if (!mz_zip_reader_init_cfile(&impl_->zip, impl_->file, (mz_uint64)size, 0))
  {
    error = "Not a valid DAWproject (ZIP) file: " + path;
    return false;
  }
  impl_->open = true;
  return true;
}

std::string DawProjectContainer::EntryName(const std::string& xmlPath)
{
  std::string name = xmlPath;
  for (char& c : name)
    if (c == '\\') c = '/';
  while (name.compare(0, 2, "./") == 0) name.erase(0, 2);
  while (!name.empty() && name[0] == '/') name.erase(0, 1);
  return name;
}

int DawProjectContainer::Locate(const std::string& name) const
{
  if (!impl_->open) return -1;
  return mz_zip_reader_locate_file(&impl_->zip, EntryName(name).c_str(), nullptr, 0);
}

bool DawProjectContainer::HasEntry(const std::string& name) const { return Locate(name) >= 0; }

int64_t DawProjectContainer::EntrySize(const std::string& name) const
{
  int idx = Locate(name);
  if (idx < 0) return -1;
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(&impl_->zip, (mz_uint)idx, &st)) return -1;
  return (int64_t)st.m_uncomp_size;
}

bool DawProjectContainer::ReadEntry(const std::string& name, std::vector<uint8_t>& out) const
{
  int idx = Locate(name);
  if (idx < 0) return false;
  size_t size = 0;
  void* p = mz_zip_reader_extract_to_heap(&impl_->zip, (mz_uint)idx, &size, 0);
  if (!p) return false;
  out.assign((uint8_t*)p, (uint8_t*)p + size);
  mz_free(p);
  return true;
}

bool DawProjectContainer::ExtractEntry(const std::string& name, const std::string& destPath) const
{
  int idx = Locate(name);
  if (idx < 0) return false;
  FILE* f = OpenFile(destPath, "wb");
  if (!f) return false;
  bool ok = mz_zip_reader_extract_to_cfile(&impl_->zip, (mz_uint)idx, f, 0) != 0;
  ok = fclose(f) == 0 && ok;
  if (!ok) RemoveFile(destPath);
  return ok;
}

}  // namespace dawp
