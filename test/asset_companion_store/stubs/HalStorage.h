#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
constexpr int O_WRONLY = 1, O_CREAT = 2, O_APPEND = 4, O_TRUNC = 8;
inline std::map<std::string, std::string> files;
inline std::set<std::string> directories;
inline std::set<std::string> failRemove, failRename;
inline std::map<std::string, int> readers;
inline size_t syncCount = 0;
inline bool failRead = false, failWrite = false, failClose = false, failSync = false, failIteration = false;
inline uint64_t totalCapacity = 64 * 1024 * 1024;
struct FsFile {
  std::string path;
  size_t pos = 0, child = 0;
  bool opened = false, directory = false, writer = false;
  std::vector<std::string> children;
  FsFile() = default;
  FsFile(const std::string& p, int flags = 0) : path(p), writer(flags & O_WRONLY) {
    if (writer && (flags & O_CREAT) && !directories.count(p)) files.try_emplace(p);
    opened = files.count(p) || directories.count(p);
    directory = directories.count(p);
    if (writer && (flags & O_TRUNC) && opened && !directory) files[p].clear();
    if (writer && (flags & O_APPEND) && opened && !directory) pos = files[p].size();
    if (directory) {
      for (const auto& v : files)
        if (v.first.substr(0, p.size() + 1) == p + "/" && v.first.find('/', p.size() + 1) == std::string::npos)
          children.push_back(v.first);
      for (const auto& v : directories)
        if (v.substr(0, p.size() + 1) == p + "/" && v.find('/', p.size() + 1) == std::string::npos)
          children.push_back(v);
    } else if (opened) {
      if (readers[p])
        opened = false;
      else
        ++readers[p];
    }
  }
  FsFile(const FsFile&) = delete;
  FsFile(FsFile&& other) { *this = std::move(other); }
  FsFile& operator=(FsFile&& other) {
    close();
    path = std::move(other.path);
    pos = other.pos;
    child = other.child;
    opened = std::exchange(other.opened, false);
    directory = other.directory;
    writer = other.writer;
    children = std::move(other.children);
    return *this;
  }
  ~FsFile() { close(); }
  explicit operator bool() const { return opened; }
  bool isDirectory() const { return directory; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return failIteration; }
  FsFile openNextFile() { return !failIteration && child < children.size() ? FsFile(children[child++]) : FsFile(); }
  size_t getName(char* out, size_t n) {
    auto name = path.substr(path.find_last_of('/') + 1);
    std::snprintf(out, n, "%s", name.c_str());
    return name.size();
  }
  uint64_t fileSize64() const { return files.at(path).size(); }
  size_t position() const { return pos; }
  bool seek(size_t offset) {
    if (!opened || directory || offset > files[path].size()) return false;
    pos = offset;
    return true;
  }
  int read(void* out, size_t n) {
    if (failRead || !opened || directory) return -1;
    n = std::min(n, files[path].size() - pos);
    std::memcpy(out, files[path].data() + pos, n);
    pos += n;
    return int(n);
  }
  size_t write(const void* in, size_t n) {
    if (failWrite || !opened || directory || !writer) return 0;
    if (pos + n > files[path].size()) files[path].resize(pos + n);
    std::memcpy(files[path].data() + pos, in, n);
    pos += n;
    return n;
  }
  bool sync() {
    ++syncCount;
    return !failSync;
  }
  bool close() {
    if (opened && !directory) --readers[path];
    opened = false;
    return !failClose;
  }
};
struct StorageFake {
  bool exists(const char* p) { return files.count(p) || directories.count(p); }
  FsFile open(const char* p, int f = 0) { return FsFile(p, f); }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    f = FsFile(p);
    return bool(f);
  }
  bool ensureDirectoryExists(const char* p) {
    std::string v = p;
    for (size_t at = 1; at <= v.size(); ++at)
      if (at == v.size() || v[at] == '/') {
        const auto d = v.substr(0, at);
        if (files.count(d)) return false;
        directories.insert(d);
      }
    return true;
  }
  bool remove(const char* p) { return !failRemove.count(p) && files.erase(p); }
  bool rmdir(const char* p) {
    if (failRemove.count(p)) return false;
    const std::string prefix = std::string(p) + "/";
    for (const auto& v : files)
      if (v.first.substr(0, prefix.size()) == prefix) return false;
    for (const auto& v : directories)
      if (v.substr(0, prefix.size()) == prefix) return false;
    return directories.erase(p);
  }
  bool rename(const char* a, const char* b) {
    if (failRename.count(a) || !exists(a) || exists(b)) return false;
    if (files.count(a)) {
      files[b] = files[a];
      files.erase(a);
      return true;
    }
    std::vector<std::pair<std::string, std::string>> moved;
    std::vector<std::string> dirs;
    std::string prefix = std::string(a) + "/";
    for (const auto& v : files)
      if (v.first.substr(0, prefix.size()) == prefix) moved.push_back(v);
    for (const auto& v : directories)
      if (v == a || v.substr(0, prefix.size()) == prefix) dirs.push_back(v);
    for (const auto& v : moved) {
      files[std::string(b) + v.first.substr(std::strlen(a))] = v.second;
      files.erase(v.first);
    }
    for (const auto& v : dirs) {
      directories.insert(std::string(b) + v.substr(std::strlen(a)));
      directories.erase(v);
    }
    return true;
  }
  uint64_t totalBytes() { return totalCapacity; }
  uint64_t usedBytes() {
    uint64_t used = directories.size() * 4096;
    for (const auto& v : files) used += v.second.size();
    return used;
  }
};
inline StorageFake Storage;
using HalFile = FsFile;
