#pragma once
#include <Print.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct HostFileData {
  std::vector<uint8_t> bytes;
  bool directory = false;
};
struct HostFaults {
  int bytesUntilFailure = -1;
  bool failSync = false;
  bool failClose = false;
  bool failScan = false;
  bool failOpenWrite = false;
  std::string failReadPath;
  std::string readErrorPath;
  size_t readErrorAfter = 0;
  bool shortRead = false;
  size_t writes = 0;
};
inline HostFaults faults;
class HalFile : public Print {
 public:
  HalFile() = default;
  HalFile(std::shared_ptr<HostFileData> data, std::string name) : data_(std::move(data)), name_(std::move(name)) {}
  int read(void* output, size_t length) {
    if (!data_) return 0;
    if (name_ == faults.readErrorPath && cursor_ >= faults.readErrorAfter) return faults.shortRead ? 0 : -1;
    const size_t n = std::min(length, data_->bytes.size() - std::min(cursor_, data_->bytes.size()));
    if (n) std::copy_n(data_->bytes.data() + cursor_, n, static_cast<uint8_t*>(output));
    cursor_ += n;
    return static_cast<int>(n);
  }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* input, size_t length) override {
    if (!data_) return 0;
    if (faults.bytesUntilFailure >= 0) {
      length = std::min(length, static_cast<size_t>(faults.bytesUntilFailure));
      faults.bytesUntilFailure -= length;
    }
    if (cursor_ + length > data_->bytes.size()) data_->bytes.resize(cursor_ + length);
    std::copy_n(input, length, data_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += length;
    return length;
  }
  size_t write(const void* input, size_t length) { return write(static_cast<const uint8_t*>(input), length); }
  bool sync() {
    if (faults.failSync) {
      faults.failSync = false;
      return false;
    }
    return bool(data_);
  }
  bool close() {
    const bool fail = data_ && !data_->directory && faults.failClose;
    if (fail) faults.failClose = false;
    data_.reset();
    cursor_ = 0;
    entries_.clear();
    return !fail;
  }
  size_t fileSize() const { return data_ ? data_->bytes.size() : 0; }
  bool isDirectory() const { return data_ && data_->directory; }
  size_t getName(char* out, size_t capacity) const {
    const std::string name = name_.substr(name_.find_last_of('/') + 1);
    if (name.size() >= capacity) return 0;
    std::memcpy(out, name.c_str(), name.size() + 1);
    return name.size();
  }
  HalFile openNextFile() {
    if (faults.failScan) {
      scanFailed_ = true;
      return {};
    }
    if (cursor_ >= entries_.size()) return {};
    auto& entry = entries_[cursor_++];
    return HalFile(entry.second, entry.first);
  }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return scanFailed_; }
  explicit operator bool() const { return bool(data_); }
  std::vector<std::pair<std::string, std::shared_ptr<HostFileData>>> entries_;

 private:
  std::shared_ptr<HostFileData> data_;
  std::string name_;
  size_t cursor_ = 0;
  bool scanFailed_ = false;
};
using FsFile = HalFile;
class HalStorage {
 public:
  void reset() {
    files.clear();
    faults = {};
    failRemovePath_.clear();
  }
  bool mkdir(const char* path) {
    auto data = std::make_shared<HostFileData>();
    data->directory = true;
    files[path] = data;
    return true;
  }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) {
    if (failRemovePath_ == path) {
      failRemovePath_.clear();
      return false;
    }
    return files.erase(path) > 0;
  }
  HalFile open(const char* path) {
    auto it = files.find(path);
    if (it == files.end()) return {};
    HalFile file(it->second, path);
    if (it->second->directory) {
      const std::string prefix = std::string(path) + "/";
      for (const auto& entry : files)
        if (entry.first.starts_with(prefix)) file.entries_.push_back(entry);
    }
    return file;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    if (faults.failReadPath == path) return false;
    const auto it = files.find(path);
    if (it == files.end()) return false;
    file = HalFile(it->second, path);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    if (faults.failOpenWrite) return false;
    auto data = std::make_shared<HostFileData>();
    files[path] = data;
    file = HalFile(data, path);
    ++faults.writes;
    return true;
  }
  void failNextRemove(std::string path) { failRemovePath_ = std::move(path); }
  std::map<std::string, std::shared_ptr<HostFileData>> files;

 private:
  std::string failRemovePath_;
};
inline HalStorage Storage;
