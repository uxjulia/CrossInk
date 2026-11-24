#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

constexpr int O_RDONLY = 0;
constexpr int O_WRONLY = 1;
constexpr int O_CREAT = 2;
constexpr int O_AT_END = 4;

struct LookupFileData {
  std::vector<uint8_t> bytes;
};

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalStorage* storage, std::string path, std::shared_ptr<LookupFileData> data, bool writable, size_t position);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  ~HalFile();

  int available() const;
  int read();
  int read(void* output, size_t length);
  size_t write(const void* input, size_t length);
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t fileSize() const;
  bool seek(size_t position);
  bool sync() const;
  bool close();
  explicit operator bool() const { return static_cast<bool>(data_); }

 private:
  HalStorage* storage_ = nullptr;
  std::string path_;
  std::shared_ptr<LookupFileData> data_;
  bool writable_ = false;
  size_t position_ = 0;
};

class HalStorage {
 public:
  HalFile open(const char* path, int flags = O_RDONLY) {
    auto found = files.find(path);
    const bool writable = (flags & O_WRONLY) != 0;
    if (writable && found == files.end() && !(flags & O_CREAT)) return {};
    if (!writable && found == files.end()) return {};
    if (writable && found == files.end()) found = files.emplace(path, std::make_shared<LookupFileData>()).first;
    if (openPaths.count(path)) return {};
    openPaths.insert(path);
    ++openHandles;
    return HalFile(this, path, found->second, writable, writable ? found->second->bytes.size() : 0);
  }

  bool exists(const char* path) const { return files.count(path) != 0; }
  bool remove(const char* path) {
    if (openPaths.count(path) || !files.count(path)) return false;
    files.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) {
    if (failRenameFrom.erase(from) || openPaths.count(from) || openPaths.count(to) || !files.count(from) ||
        files.count(to))
      return false;
    files[to] = std::move(files[from]);
    files.erase(from);
    return true;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path, O_RDONLY);
    return static_cast<bool>(file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    if (openPaths.count(path)) return false;
    files[path] = std::make_shared<LookupFileData>();
    openPaths.insert(path);
    ++openHandles;
    file = HalFile(this, path, files[path], true, 0);
    return true;
  }
  void reset() {
    files.clear();
    openPaths.clear();
    openHandles = 0;
    failSync = false;
    failClose = false;
    failWrites = false;
    writeLimit = 0;
  }

  std::map<std::string, std::shared_ptr<LookupFileData>> files;
  std::set<std::string> openPaths;
  int openHandles = 0;
  bool failSync = false;
  bool failClose = false;
  bool failWrites = false;
  std::set<std::string> failRenameFrom;
  size_t writeLimit = 0;
};

inline HalFile::HalFile(HalStorage* storage, std::string path, std::shared_ptr<LookupFileData> data, bool writable,
                        size_t position)
    : storage_(storage), path_(std::move(path)), data_(std::move(data)), writable_(writable), position_(position) {}
inline HalFile::HalFile(HalFile&& other) noexcept { *this = std::move(other); }
inline HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    close();
    storage_ = other.storage_;
    path_ = std::move(other.path_);
    data_ = std::move(other.data_);
    writable_ = other.writable_;
    position_ = other.position_;
    other.storage_ = nullptr;
  }
  return *this;
}
inline HalFile::~HalFile() { close(); }
inline int HalFile::available() const {
  return data_ ? static_cast<int>(data_->bytes.size() - std::min(position_, data_->bytes.size())) : 0;
}
inline int HalFile::read() {
  if (!data_ || position_ >= data_->bytes.size()) return -1;
  return data_->bytes[position_++];
}
inline int HalFile::read(void* output, size_t length) {
  if (!data_) return -1;
  const size_t count = std::min(length, data_->bytes.size() - std::min(position_, data_->bytes.size()));
  std::copy_n(data_->bytes.data() + position_, count, static_cast<uint8_t*>(output));
  position_ += count;
  return static_cast<int>(count);
}
inline size_t HalFile::write(const void* input, size_t length) {
  if (!data_ || !writable_ || storage_->failWrites) return 0;
  const size_t count = storage_->writeLimit ? std::min(length, storage_->writeLimit) : length;
  if (position_ + count > data_->bytes.size()) data_->bytes.resize(position_ + count);
  std::copy_n(static_cast<const uint8_t*>(input), count, data_->bytes.data() + position_);
  position_ += count;
  if (storage_->writeLimit) storage_->writeLimit = 0;
  return count;
}
inline size_t HalFile::fileSize() const { return data_ ? data_->bytes.size() : 0; }
inline bool HalFile::seek(size_t position) {
  if (!data_ || position > data_->bytes.size()) return false;
  position_ = position;
  return true;
}
inline bool HalFile::sync() const { return data_ && !storage_->failSync; }
inline bool HalFile::close() {
  if (data_ && storage_) {
    storage_->openPaths.erase(path_);
    --storage_->openHandles;
  }
  data_.reset();
  return !storage_ || !storage_->failClose;
}

inline HalStorage Storage;
