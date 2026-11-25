#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class HalFile {
  FILE* file_ = nullptr;
  std::string path_;
  std::vector<std::string> children_;
  size_t child_ = 0;
  bool directory_ = false;

 public:
  inline static size_t reads = 0, seeks = 0, liveFiles = 0, syncs = 0;
  inline static bool failOpen = false, failRead = false, failClose = false;
  HalFile() = default;
  explicit HalFile(std::string path, int flags = O_RDONLY) : path_(std::move(path)) {
    if (failOpen) return;
    std::error_code ec;
    directory_ = std::filesystem::is_directory(path_, ec);
    if (directory_)
      for (const auto& child : std::filesystem::directory_iterator(path_)) children_.push_back(child.path());
    else {
      const int fd = ::open(path_.c_str(), flags, 0600);
      if (fd >= 0) file_ = ::fdopen(fd, (flags & O_RDWR) ? "r+b" : "rb");
      if (file_) ++liveFiles;
    }
  }
  ~HalFile() { close(); }
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      file_ = std::exchange(other.file_, nullptr);
      path_ = std::move(other.path_);
      children_ = std::move(other.children_);
      child_ = other.child_;
      directory_ = std::exchange(other.directory_, false);
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  int read(void* dest, size_t count) {
    ++reads;
    if (failRead) return -1;
    const size_t n = std::fread(dest, 1, count, file_);
    return std::ferror(file_) ? -1 : int(n);
  }
  size_t write(const void* bytes, size_t length) { return file_ ? std::fwrite(bytes, 1, length, file_) : 0; }
  bool sync() {
    assert(file_);
    ++syncs;
    const bool flushed = std::fflush(file_) == 0;
    const char* failAt = std::getenv("CROSSINK_TEST_FAIL_SYNC");
    return flushed && (!failAt || syncs != std::strtoul(failAt, nullptr, 10));
  }
  size_t position() const {
    assert(file_);
    return static_cast<size_t>(std::ftell(file_));
  }
  bool seek(size_t offset) {
    ++seeks;
    return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
  }
  size_t fileSize() const {
    std::error_code ec;
    return std::filesystem::file_size(path_, ec);
  }
  bool isDirectory() const { return directory_; }
  size_t getName(char* out, size_t length) const {
    const auto name = std::filesystem::path(path_).filename().string();
    std::snprintf(out, length, "%s", name.c_str());
    return name.size();
  }
  bool allocationFailed() const { return std::getenv("CROSSINK_TEST_DIRECTORY_OOM") != nullptr; }
  HalFile openNextFile() {
    if (allocationFailed()) return {};
    return child_ < children_.size() ? HalFile(children_[child_++]) : HalFile();
  }
  bool iterationFailed() const { return false; }
  bool close() {
    bool ok = true;
    if (file_) {
      ok = std::fclose(file_) == 0;
      if (failClose) ok = false;
      file_ = nullptr;
      --liveFiles;
    }
    directory_ = false;
    return ok;
  }
  explicit operator bool() const { return file_ || directory_; }
};
class HalStorage {
 public:
  inline static size_t opens = 0;
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  HalFile open(const char* path, int flags = O_RDONLY) {
    ++opens;
    const char* root = std::getenv("CROSSINK_TEST_SD");
    return HalFile(std::string(root ? root : ".") + path, flags);
  }
  bool exists(const char* path) {
    const char* root = std::getenv("CROSSINK_TEST_SD");
    return std::filesystem::exists(std::string(root ? root : ".") + path);
  }
  bool ensureDirectoryExists(const char* path) {
    const char* root = std::getenv("CROSSINK_TEST_SD");
    std::error_code ec;
    std::filesystem::create_directories(std::string(root ? root : ".") + path, ec);
    return !ec;
  }
  bool rename(const char* from, const char* to) {
    const std::string root = std::getenv("CROSSINK_TEST_SD");
    return std::rename((root + from).c_str(), (root + to).c_str()) == 0;
  }
  bool remove(const char* path) {
    const char* root = std::getenv("CROSSINK_TEST_SD");
    std::error_code ec;
    return std::filesystem::remove(std::string(root ? root : ".") + path, ec);
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path);
    return bool(file);
  }
};
#define Storage HalStorage::getInstance()
