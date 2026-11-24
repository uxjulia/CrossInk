#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct StorageStub {
  std::map<std::string, std::vector<unsigned char>> files;
  bool shortWrite = false;
  bool shortRead = false;
  bool failSync = false;
  bool failClose = false;
  bool failOpen = false;
  bool failRemove = false;
  int openHandles = 0;
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) {
    if (failRemove || openHandles) return false;
    return files.erase(path) > 0;
  }
  bool rename(const char* from, const char* to) {
    if (openHandles || !files.contains(from) || files.contains(to)) return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, class FsFile& file);
  bool openFileForRead(const char*, const std::string& path, class FsFile& file);
};
inline StorageStub Storage;
class FsFile {
 public:
  std::vector<unsigned char>* bytes = nullptr;
  size_t size() { return bytes->size(); }
  size_t write(const void* data, size_t size) {
    if (Storage.shortWrite) --size;
    const auto* p = static_cast<const unsigned char*>(data);
    bytes->assign(p, p + size);
    return size;
  }
  int read(void* data, size_t size) {
    size = std::min(size, bytes->size());
    if (Storage.shortRead && size) --size;
    std::memcpy(data, bytes->data(), size);
    return static_cast<int>(size);
  }
  bool sync() { return !Storage.failSync; }
  bool close() {
    if (bytes) --Storage.openHandles;
    bytes = nullptr;
    return !Storage.failClose;
  }
};
inline bool StorageStub::openFileForWrite(const char*, const std::string& path, FsFile& file) {
  if (failOpen || openHandles) return false;
  files[path].clear();
  file.bytes = &files[path];
  ++openHandles;
  return true;
}
inline bool StorageStub::openFileForRead(const char*, const std::string& path, FsFile& file) {
  if (failOpen || openHandles || !files.contains(path)) return false;
  file.bytes = &files[path];
  ++openHandles;
  return true;
}

using HalFile = FsFile;
