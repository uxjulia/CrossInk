#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using oflag_t = uint8_t;
inline constexpr oflag_t O_RDONLY = 0;
inline constexpr oflag_t O_RDWR = 1;
inline constexpr oflag_t O_CREAT = 2;

struct HostFileData {
  std::vector<uint8_t> bytes;
  size_t reads = 0;
  size_t seeks = 0;
  size_t failAt = std::numeric_limits<size_t>::max();
  size_t failReadAt = std::numeric_limits<size_t>::max();
  size_t failReadCount = 0;
  size_t corruptReadAt = std::numeric_limits<size_t>::max();
  size_t corruptReadLength = 0;
  size_t corruptReadCount = 0;
  uint8_t corruptReadValue = 0;
  size_t failSeekAt = std::numeric_limits<size_t>::max();
  size_t failSeekCount = 0;
  bool failSync = false;
  bool failClose = false;
};

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<HostFileData> data) : data_(std::move(data)) {}

  void flush() {}
  size_t size() const { return data_ ? data_->bytes.size() : 0; }
  size_t fileSize() const { return size(); }
  int available() const {
    if (!data_ || cursor_ >= data_->bytes.size()) return 0;
    return static_cast<int>(
        std::min(data_->bytes.size() - cursor_, static_cast<size_t>(std::numeric_limits<int>::max())));
  }
  size_t position() const { return cursor_; }

  int read(void* output, const size_t length) {
    if (!data_) return 0;
    ++data_->reads;
    if (data_->failReadCount > 0 && cursor_ >= data_->failReadAt) {
      --data_->failReadCount;
      return -1;
    }
    const size_t readable = std::min(length, data_->bytes.size() - std::min(cursor_, data_->bytes.size()));
    const size_t readStart = cursor_;
    if (readable > 0) std::copy_n(data_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    if (data_->corruptReadCount > 0 && readable > 0) {
      const size_t readEnd = readStart + readable;
      const size_t corruptEnd = data_->corruptReadAt + data_->corruptReadLength;
      const size_t overlapStart = std::max(readStart, data_->corruptReadAt);
      const size_t overlapEnd = std::min(readEnd, corruptEnd);
      if (overlapStart < overlapEnd) {
        auto* bytes = static_cast<uint8_t*>(output);
        std::fill(bytes + (overlapStart - readStart), bytes + (overlapEnd - readStart), data_->corruptReadValue);
        --data_->corruptReadCount;
      }
    }
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!data_ || position > data_->bytes.size()) return false;
    if (data_->failSeekCount > 0 && position == data_->failSeekAt) {
      --data_->failSeekCount;
      return false;
    }
    ++data_->seeks;
    cursor_ = position;
    return true;
  }
  bool seekSet(const size_t position) { return seek(position); }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* input, const size_t length) override {
    if (!data_ || cursor_ >= data_->failAt) return 0;
    const size_t writable = std::min(length, data_->failAt - cursor_);
    if (cursor_ + writable > data_->bytes.size()) data_->bytes.resize(cursor_ + writable);
    if (writable > 0) std::copy_n(input, writable, data_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += writable;
    return writable;
  }
  size_t write(const void* input, const size_t length) { return write(static_cast<const uint8_t*>(input), length); }

  bool sync() const { return data_ && !data_->failSync; }
  bool close() {
    const bool success = data_ && !data_->failClose;
    data_.reset();
    cursor_ = 0;
    return success;
  }
  bool isOpen() const { return static_cast<bool>(data_); }
  explicit operator bool() const { return isOpen(); }

 private:
  std::shared_ptr<HostFileData> data_;
  size_t cursor_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() {
    files_.clear();
    writeOpens = removes = renames = 0;
    failWrite = false;
    failRenameFrom.clear();
    failSyncPath.clear();
    failClosePath.clear();
  }
  size_t writeOpens = 0;
  size_t removes = 0;
  size_t renames = 0;
  bool failWrite = false;
  std::string failRenameFrom;
  std::string failSyncPath;
  std::string failClosePath;
  size_t reads(const std::string& path) const { return files_.at(path)->reads; }
  size_t seeks(const std::string& path) const { return files_.at(path)->seeks; }
  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files_.contains(path); }
  bool remove(const char* path) {
    ++removes;
    return files_.erase(path) > 0;
  }
  bool rename(const char* from, const char* to) {
    ++renames;
    if (failRenameFrom == from) {
      failRenameFrom.clear();
      return false;
    }
    const auto found = files_.find(from);
    if (found == files_.end() || files_.contains(to)) return false;
    files_[to] = found->second;
    files_.erase(found);
    return true;
  }

  HalFile open(const char* path, oflag_t flags) {
    if (!files_.contains(path) && (flags & O_CREAT)) files_[path] = std::make_shared<HostFileData>();
    const auto found = files_.find(path);
    if (found != files_.end() && (flags & O_RDWR)) {
      ++writeOpens;
      found->second->failAt = failWrite ? 0 : std::numeric_limits<size_t>::max();
      failWrite = false;
    }
    return found == files_.end() ? HalFile{} : HalFile(found->second);
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = open(path.c_str(), O_RDONLY);
    return static_cast<bool>(file);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    ++writeOpens;
    auto data = std::make_shared<HostFileData>();
    if (failWrite) {
      data->failAt = 0;
      failWrite = false;
    }
    data->failSync = path == failSyncPath;
    data->failClose = path == failClosePath;
    files_[path] = data;
    file = HalFile(std::move(data));
    return true;
  }

  void failWritesAt(const std::string& path, size_t offset) { files_.at(path)->failAt = offset; }
  void failReadsAt(const std::string& path, size_t offset) {
    files_.at(path)->failReadAt = offset;
    files_.at(path)->failReadCount = 1;
  }
  void corruptReadsAt(const std::string& path, size_t offset, size_t length, uint8_t value) {
    files_.at(path)->corruptReadAt = offset;
    files_.at(path)->corruptReadLength = length;
    files_.at(path)->corruptReadValue = value;
    files_.at(path)->corruptReadCount = 1;
  }
  void failSeeksAt(const std::string& path, size_t offset) {
    files_.at(path)->failSeekAt = offset;
    files_.at(path)->failSeekCount = 1;
  }
  void failSyncAt(const std::string& path) { files_.at(path)->failSync = true; }
  void failCloseAt(const std::string& path) { files_.at(path)->failClose = true; }
  const std::vector<uint8_t>& bytes(const std::string& path) const { return files_.at(path)->bytes; }
  void corruptByteAt(const std::string& path, const size_t offset, const uint8_t value) {
    files_.at(path)->bytes.at(offset) = value;
  }
  void put(const std::string& path, std::vector<uint8_t> bytes) {
    auto data = std::make_shared<HostFileData>();
    data->bytes = std::move(bytes);
    files_[path] = std::move(data);
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<HostFileData>> files_;
};

#define Storage HalStorage::getInstance()
