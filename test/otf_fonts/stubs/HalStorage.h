#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
inline std::string testRoot;
inline bool failDirectoryScan = false;
inline bool failFontRead = false;
inline bool failDirectoryWrapperAlloc = false;
inline bool failOpenWrapperAlloc = false;
inline unsigned directorySteps = 0;
class HalFile {
  std::string path_;
  FILE* file_ = nullptr;
  std::filesystem::directory_iterator iterator_, end_;
  bool directory_ = false, failed_ = false, allocationFailed_ = false;

 public:
  HalFile() = default;
  static HalFile wrapperAllocationFailure() {
    HalFile failed;
    failed.allocationFailed_ = true;
    return failed;
  }
  explicit HalFile(std::string path, bool write = false) : path_(std::move(path)) {
    directory_ = std::filesystem::is_directory(path_);
    if (directory_)
      iterator_ = std::filesystem::directory_iterator(path_);
    else
      file_ = std::fopen(path_.c_str(), write ? "w+b" : "rb");
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    close();
    path_ = std::move(other.path_);
    file_ = std::exchange(other.file_, nullptr);
    directory_ = std::exchange(other.directory_, false);
    failed_ = std::exchange(other.failed_, false);
    allocationFailed_ = std::exchange(other.allocationFailed_, false);
    iterator_ = std::move(other.iterator_);
    return *this;
  }
  ~HalFile() { close(); }
  bool close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    directory_ = false;
    failed_ = false;
    allocationFailed_ = false;
    return true;
  }
  explicit operator bool() const { return file_ || directory_; }
  bool isDirectory() const { return directory_; }
  bool allocationFailed() const { return allocationFailed_; }
  bool iterationFailed() const { return failed_; }
  HalFile openNextFile() {
    ++directorySteps;
    allocationFailed_ = false;
    if (failDirectoryWrapperAlloc) {
      allocationFailed_ = true;
      return {};
    }
    if (failDirectoryScan) {
      failed_ = true;
      return {};
    }
    if (!directory_ || iterator_ == end_) return {};
    auto path = iterator_->path();
    ++iterator_;
    return HalFile(path.string());
  }
  size_t getName(char* out, size_t cap) {
    auto name = std::filesystem::path(path_).filename().string();
    std::snprintf(out, cap, "%s", name.c_str());
    return std::min(cap - 1, name.size());
  }
  uint64_t fileSize64() { return directory_ ? 0 : std::filesystem::file_size(path_); }
  size_t size() {
    if (!file_) return 0;
    auto pos = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    auto n = std::ftell(file_);
    std::fseek(file_, pos, SEEK_SET);
    return n;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  bool seek(size_t offset) { return file_ && offset <= size() && std::fseek(file_, offset, SEEK_SET) == 0; }
  int read(void* data, size_t length) { return file_ && !failFontRead ? std::fread(data, 1, length, file_) : 0; }
  size_t write(const void* data, size_t length) { return file_ ? std::fwrite(data, 1, length, file_) : 0; }
  bool sync() { return file_ && std::fflush(file_) == 0; }
};
class TestStorage {
 public:
  HalFile open(const char* path) {
    return failOpenWrapperAlloc ? HalFile::wrapperAllocationFailure() : HalFile(testRoot + path);
  }
  bool exists(const char* path) { return std::filesystem::exists(testRoot + path); }
  bool mkdir(const char* path, bool = true) {
    return std::filesystem::create_directories(testRoot + path) || exists(path);
  }
  bool remove(const char* path) { return std::filesystem::remove(testRoot + path); }
  bool rename(const char* src, const char* dst) {
    std::error_code e;
    std::filesystem::rename(testRoot + src, testRoot + dst, e);
    return !e;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = HalFile(testRoot + path);
    return bool(file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = HalFile(testRoot + path, true);
    return bool(file);
  }
};
inline TestStorage Storage;
