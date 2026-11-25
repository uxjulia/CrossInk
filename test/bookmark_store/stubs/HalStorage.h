#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

inline int openBookmarkReaders = 0;
inline int shortReadAfter = -1;
inline bool bookmarkMockWriting = false;
inline bool failBookmarkWriteOpen = false;
inline bool failBookmarkSync = false;
inline bool failBookmarkClose = false;
inline int maxBookmarkWriteBytes = -1;
inline std::string failBookmarkRenameFrom;

class FsFile {
 public:
  bool attach(std::string* bytes, bool writing) {
    if (!writing && openBookmarkReaders != 0) return false;
    data = bytes;
    writer = writing;
    bookmarkMockWriting = writing;
    offset = 0;
    if (!writer) ++openBookmarkReaders;
    return true;
  }
  int available() const { return data ? static_cast<int>(data->size() - offset) : 0; }
  int read(void* bytes, size_t size) {
    size = std::min(size, static_cast<size_t>(available()));
    if (shortReadAfter >= 0 && offset + size > static_cast<size_t>(shortReadAfter)) return 0;
    if (size) std::memcpy(bytes, data->data() + offset, size);
    offset += size;
    return static_cast<int>(size);
  }
  size_t write(const uint8_t* bytes, size_t size) {
    if (!data || !writer) return 0;
    if (maxBookmarkWriteBytes >= 0) size = std::min(size, static_cast<size_t>(maxBookmarkWriteBytes));
    data->append(reinterpret_cast<const char*>(bytes), size);
    return size;
  }
  bool sync() { return !failBookmarkSync; }
  bool close() {
    const bool ok = !data || !writer || !failBookmarkClose;
    if (data && !writer) --openBookmarkReaders;
    data = nullptr;
    bookmarkMockWriting = false;
    return ok;
  }

 private:
  std::string* data = nullptr;
  size_t offset = 0;
  bool writer = false;
};

class HalStorage {
 public:
  std::map<std::string, std::string> files;
  size_t writes = 0;
  bool exists(const char* path) const {
    if (files.contains(path)) return true;
    const std::string prefix = std::string(path) + "/";
    const auto it = files.lower_bound(prefix);
    return it != files.end() && it->first.starts_with(prefix);
  }
  bool mkdir(const char*, bool = true) { return true; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (failBookmarkRenameFrom == from) return false;
    if (!files.contains(from) || files.contains(to)) return false;
    auto node = files.extract(from);
    node.key() = to;
    files.insert(std::move(node));
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, FsFile& file) {
    auto it = files.find(path);
    return it != files.end() && file.attach(&it->second, false);
  }
  bool openFileForWrite(const char*, const std::string& path, FsFile& file) {
    ++writes;
    files[path].clear();
    if (failBookmarkWriteOpen) return false;  // Match O_TRUNC before HAL wrapper OOM.
    return file.attach(&files[path], true);
  }
  std::vector<std::string> listFiles(const char* directory, size_t limit = SIZE_MAX) const {
    std::vector<std::string> names;
    const std::string prefix = std::string(directory) + "/";
    for (const auto& [path, bytes] : files) {
      if (path.starts_with(prefix) && names.size() < limit) names.push_back(path.substr(prefix.size()));
    }
    return names;
  }
};
inline HalStorage Storage;
