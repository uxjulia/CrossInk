#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

inline constexpr int O_RDWR = 1;
inline constexpr int O_CREAT = 2;

struct HostFileData {
  size_t writeLimit = SIZE_MAX;
  bool failSync = false;
  bool failClose = false;
  std::vector<uint8_t> bytes;
};

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<HostFileData> data) : data_(std::move(data)) {}

  int read(void* output, const size_t length) {
    if (!data_) return 0;
    const size_t readable = std::min(length, data_->bytes.size() - std::min(cursor_, data_->bytes.size()));
    if (readable > 0) std::copy_n(data_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* input, const size_t length) override {
    if (!data_) return 0;
    const size_t n = std::min(length, data_->writeLimit);
    if (cursor_ + n > data_->bytes.size()) data_->bytes.resize(cursor_ + n);
    std::copy_n(input, n, data_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += n;
    return n;
  }
  size_t write(const void* input, const size_t length) { return write(static_cast<const uint8_t*>(input), length); }

  void flush() {}
  bool sync() const { return data_ && !data_->failSync; }
  bool seekSet(size_t offset) {
    if (!data_ || offset > data_->bytes.size()) return false;
    cursor_ = offset;
    return true;
  }
  bool close() {
    const bool ok = !data_ || !data_->failClose;
    data_.reset();
    cursor_ = 0;
    return ok;
  }
  size_t fileSize() const { return data_ ? data_->bytes.size() : 0; }
  explicit operator bool() const { return static_cast<bool>(data_); }

 private:
  std::shared_ptr<HostFileData> data_;
  size_t cursor_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  void reset() {
    files_.clear();
    failRenameFrom_.clear();
    failRemovePath_.clear();
  }

  bool exists(const char* path) const { return files_.contains(path); }
  bool remove(const char* path) {
    if (failRemovePath_ == path) {
      failRemovePath_.clear();
      return false;
    }
    return files_.erase(path) > 0;
  }

  bool rename(const char* from, const char* to) {
    if (failRenameFrom_.erase(from) > 0) {
      return false;
    }
    const auto source = files_.find(from);
    if (source == files_.end() || files_.contains(to)) return false;
    files_[to] = source->second;
    files_.erase(source);
    return true;
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    const auto found = files_.find(path);
    if (found == files_.end()) return false;
    file = HalFile(found->second);
    return true;
  }

  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    auto data = std::make_shared<HostFileData>();
    files_[path] = data;
    file = HalFile(std::move(data));
    return true;
  }

  HalFile open(const char* path, int flags) {
    auto found = files_.find(path);
    if (found == files_.end() && (flags & O_CREAT)) files_[path] = std::make_shared<HostFileData>();
    found = files_.find(path);
    return found == files_.end() ? HalFile{} : HalFile(found->second);
  }
  HostFileData& data(const std::string& path) { return *files_.at(path); }
  void failNextRenameFrom(std::string path) { failRenameFrom_.insert(std::move(path)); }
  void failNextRemove(std::string path) { failRemovePath_ = std::move(path); }

 private:
  std::unordered_map<std::string, std::shared_ptr<HostFileData>> files_;
  std::unordered_set<std::string> failRenameFrom_;
  std::string failRemovePath_;
};

inline HalStorage Storage;
