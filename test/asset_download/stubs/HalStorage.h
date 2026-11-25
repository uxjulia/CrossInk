#pragma once
#include <fcntl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
inline std::map<std::string, std::string> files;
inline std::set<std::string> directories;
inline std::set<std::string> failRemove, failRename;
inline bool failClose = false, failRead = false, failIteration = false;
inline std::map<std::string, int> openReaders;
class HalFile {
 public:
  std::string path;
  size_t position_ = 0, child = 0;
  bool opened = false, directory = false;
  std::vector<std::string> children;
  HalFile() = default;
  explicit HalFile(const std::string& p)
      : path(p),
        opened(p == "/" || files.count(p) || directories.count(p)),
        directory(p == "/" || directories.count(p)) {
    if (directory) {
      const std::string prefix = p == "/" ? p : p + "/";
      for (const auto& kv : files)
        if (kv.first.substr(0, prefix.size()) == prefix && kv.first.find('/', prefix.size()) == std::string::npos)
          children.push_back(kv.first);
      for (const auto& d : directories)
        if (d.substr(0, prefix.size()) == prefix && d.find('/', prefix.size()) == std::string::npos)
          children.push_back(d);
    } else if (opened) {
      // Hardware does not support concurrent readers of one path.
      if (openReaders[p])
        opened = false;
      else
        ++openReaders[p];
    }
  }
  HalFile(const HalFile&) = delete;
  HalFile(HalFile&& o) { *this = std::move(o); }
  HalFile& operator=(HalFile&& o) {
    close();
    path = std::move(o.path);
    position_ = o.position_;
    child = o.child;
    opened = std::exchange(o.opened, false);
    directory = o.directory;
    children = std::move(o.children);
    return *this;
  }
  ~HalFile() { close(); }
  operator bool() const { return opened; }
  bool isDirectory() const { return directory; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return failIteration; }
  HalFile openNextFile() { return !failIteration && child < children.size() ? HalFile(children[child++]) : HalFile(); }
  size_t getName(char* out, size_t size) {
    const auto name = path.substr(path.find_last_of('/') + 1);
    std::snprintf(out, size, "%s", name.c_str());
    return name.size();
  }
  size_t fileSize() const { return files.at(path).size(); }
  uint64_t fileSize64() const { return files.at(path).size(); }
  size_t position() const { return position_; }
  bool seek(size_t offset) {
    if (!opened || directory || offset > files.at(path).size()) return false;
    position_ = offset;
    return true;
  }
  int read(void* out, size_t size) {
    if (failRead || !opened || directory) return -1;
    size = std::min(size, files[path].size() - position_);
    std::memcpy(out, files[path].data() + position_, size);
    position_ += size;
    return int(size);
  }
  size_t write(const void* bytes, size_t size) {
    if (!opened || directory) return 0;
    files[path].resize(std::max(files[path].size(), position_ + size));
    std::memcpy(files[path].data() + position_, bytes, size);
    position_ += size;
    return size;
  }
  bool sync() { return opened && !failClose; }
  bool close() {
    if (opened && !directory) --openReaders[path];
    opened = false;
    return !failClose;
  }
};
struct StorageFake {
  bool openFileForWrite(const char*, const char* p, HalFile& file) {
    files[p].clear();
    file = HalFile(p);
    return bool(file);
  }
  bool readFileToString(const char*, const std::string& p, size_t cap, std::string& out) {
    if (failRead || !files.count(p) || files[p].size() > cap) return false;
    out = files[p];
    return true;
  }
  bool exists(const char* p) { return files.count(p) || directories.count(p); }
  HalFile open(const char* p, int flags = O_RDONLY) {
    if (flags & O_CREAT) {
      if ((flags & O_EXCL) && exists(p)) return {};
      files.try_emplace(p);
    }
    return HalFile(p);
  }
  bool openFileForRead(const char*, const char* p, HalFile& f) {
    f = HalFile(p);
    return bool(f);
  }
  bool ensureDirectoryExists(const char* p) {
    std::string path(p);
    for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
      directories.insert(path.substr(0, slash));
    directories.insert(path);
    return true;
  }
  bool remove(const char* p) { return !failRemove.count(p) && files.erase(p); }
  bool rename(const char* a, const char* b) {
    if (failRename.count(a) || !files.count(a) || exists(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
};
inline StorageFake Storage;

using FsFile = HalFile;
