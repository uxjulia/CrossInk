#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

inline constexpr int O_WRONLY = 1;
inline constexpr int O_CREAT = 2;
inline constexpr int O_TRUNC = 4;

struct OutboxTestFile {
  std::string data;
  bool opened = false;
  bool directory = false;
};

struct OutboxTestStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(OutboxTestStorage& storage, OutboxTestFile& file, bool write)
      : storage(&storage), file(&file), writing(write) {
    file.opened = true;
  }

  explicit operator bool() const { return file != nullptr; }
  bool isDirectory() const { return file->directory; }
  size_t fileSize() const { return file->data.size(); }
  int read(void* buffer, size_t length);
  size_t write(const uint8_t* data, size_t length);
  bool sync() const;
  bool close();

 private:
  OutboxTestStorage* storage = nullptr;
  OutboxTestFile* file = nullptr;
  size_t offset = 0;
  bool writing = false;
};

struct OutboxTestStorage {
  std::map<std::string, OutboxTestFile> files;
  std::string failRenameFrom, failRemovePath;
  bool failReadOpen = false, failOutputOpen = false, failSync = false;
  bool failSourceClose = false, failOutputClose = false;
  size_t maxReadable = SIZE_MAX, maxWritable = SIZE_MAX, biggestRead = 0;

  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) {
    return path != failRemovePath && exists(path) && !files.at(path).opened && files.erase(path) == 1;
  }
  bool rename(const char* from, const char* to) {
    if (from == failRenameFrom || !exists(from) || files.at(from).opened || exists(to)) return false;
    auto node = files.extract(from);
    node.key() = to;
    files.insert(std::move(node));
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& out) {
    if (failReadOpen || !exists(path) || files.at(path).opened) return false;
    out = HalFile(*this, files.at(path), false);
    return true;
  }
  HalFile open(const char* path, const int flags) {
    if (failOutputOpen || flags != (O_WRONLY | O_CREAT | O_TRUNC) || (exists(path) && files.at(path).opened)) return {};
    auto& f = files[path];
    f.data.clear();
    return HalFile(*this, f, true);
  }
};
inline OutboxTestStorage Storage;

inline int HalFile::read(void* buffer, const size_t length) {
  storage->biggestRead = std::max(storage->biggestRead, length);
  const size_t stop = std::min(file->data.size(), storage->maxReadable);
  const size_t read = std::min(length, stop - offset);
  memcpy(buffer, file->data.data() + offset, read);
  offset += read;
  return static_cast<int>(read);
}

inline size_t HalFile::write(const uint8_t* data, const size_t length) {
  const size_t written = std::min(length, storage->maxWritable - file->data.size());
  file->data.append(reinterpret_cast<const char*>(data), written);
  return written;
}

inline bool HalFile::sync() const { return !storage->failSync; }

inline bool HalFile::close() {
  if (!file) return true;
  file->opened = false;
  file = nullptr;
  return writing ? !storage->failOutputClose : !storage->failSourceClose;
}
