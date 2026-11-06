#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

inline int openBookmarkReaders = 0;
inline void (*bookmarkReadHook)() = nullptr;
inline int shortReadAfter = -1;
inline bool bookmarkMockWriting = false;
inline bool failBookmarkWriteOpen = false;
inline bool failBookmarkSync = false;
inline bool failBookmarkClose = false;
inline int maxBookmarkWriteBytes = -1;
inline std::string failBookmarkRenameFrom;

constexpr int O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4;
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
    if (bookmarkReadHook) bookmarkReadHook();
    size = std::min(size, static_cast<size_t>(available()));
    if (shortReadAfter >= 0 && offset + size > static_cast<size_t>(shortReadAfter)) return 0;
    if (size) std::memcpy(bytes, data->data() + offset, size);
    offset += size;
    return static_cast<int>(size);
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t write(const void* bytes, size_t size) { return write(static_cast<const uint8_t*>(bytes), size); }
  size_t write(const uint8_t* bytes, size_t size) {
    if (!data || !writer) return 0;
    if (maxBookmarkWriteBytes >= 0) size = std::min(size, static_cast<size_t>(maxBookmarkWriteBytes));
    data->append(reinterpret_cast<const char*>(bytes), size);
    offset += size;
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

  explicit operator bool() const { return data != nullptr; }
  size_t position() const { return offset; }
  size_t fileSize() const { return data ? data->size() : 0; }
  size_t size() const { return fileSize(); }
  bool seek(size_t pos) {
    if (!data || pos > data->size()) return false;
    offset = pos;
    return true;
  }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(size_t n) { return seek(offset + n); }
  bool isDirectory() const { return false; }
  FsFile openNextFile() { return {}; }
  size_t getName(char*, size_t) { return 0; }

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
  FsFile open(const char* path, int mode = 0) {
    FsFile file;
    if (mode)
      openFileForWrite("test", path, file);
    else
      openFileForRead("test", path, file);
    return file;
  }
  bool removeDir(const char* directory) {
    const std::string prefix = std::string(directory) + "/";
    for (auto it = files.begin(); it != files.end();) {
      if (it->first == directory || it->first.starts_with(prefix))
        it = files.erase(it);
      else
        ++it;
    }
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
