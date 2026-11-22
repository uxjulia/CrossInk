#pragma once

#include <Arduino.h>
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

struct HostFileData {
  std::vector<uint8_t> bytes;
  size_t readCalls = 0, readBytes = 0;
  uint32_t modified = 1;
  bool openFail = false;
  size_t readFailAt = std::numeric_limits<size_t>::max();
  size_t failAt = std::numeric_limits<size_t>::max();
};

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<HostFileData> data) : data_(std::move(data)) {}

  void flush() {}
  size_t size() const { return data_ ? data_->bytes.size() : 0; }
  size_t fileSize() const { return size(); }
  int available() const {
    return data_ ? static_cast<int>(data_->bytes.size() - std::min(cursor_, data_->bytes.size())) : 0;
  }
  size_t position() const { return cursor_; }

  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  bool seekCur(int offset) { return seek(cursor_ + offset); }

  int read(void* output, const size_t length) {
    if (!data_) return 0;
    ++data_->readCalls;
    const size_t end = std::min(data_->bytes.size(), data_->readFailAt);
    const size_t readable = std::min(length, end - std::min(cursor_, end));
    data_->readBytes += readable;
    if (readable > 0) std::copy_n(data_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!data_ || position > data_->bytes.size()) return false;
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

  bool sync() const { return static_cast<bool>(data_); }
  bool close() {
    data_.reset();
    cursor_ = 0;
    return true;
  }
  uint32_t modificationTime() const { return data_ ? data_->modified : 0; }
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

  void reset() { files_.clear(); }
  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files_.contains(path); }
  bool remove(const char* path) { return files_.erase(path) > 0; }
  bool rename(const char* from, const char* to) {
    const auto found = files_.find(from);
    if (found == files_.end() || files_.contains(to)) return false;
    files_[to] = found->second;
    files_.erase(found);
    return true;
  }

  HalFile open(const char* path, oflag_t) {
    const auto found = files_.find(path);
    return found == files_.end() || found->second->openFail ? HalFile{} : HalFile(found->second);
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = open(path.c_str(), O_RDONLY);
    return static_cast<bool>(file);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    auto data = std::make_shared<HostFileData>();
    files_[path] = data;
    file = HalFile(std::move(data));
    return true;
  }

  long openHandles(const std::string& path) const { return files_.at(path).use_count() - 1; }
  HostFileData& data(const std::string& path) { return *files_.at(path); }
  void failWritesAt(const std::string& path, size_t offset) { files_.at(path)->failAt = offset; }
  const std::vector<uint8_t>& bytes(const std::string& path) const { return files_.at(path)->bytes; }
  void put(const std::string& path, std::vector<uint8_t> bytes) {
    auto data = std::make_shared<HostFileData>();
    data->bytes = std::move(bytes);
    files_[path] = std::move(data);
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<HostFileData>> files_;
};

#define Storage HalStorage::getInstance()
