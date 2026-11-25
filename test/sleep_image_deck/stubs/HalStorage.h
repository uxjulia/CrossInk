#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace test_storage {
inline std::string root;
inline std::string failWritePath, failRenameFrom;
inline bool failSync = false, failClose = false, failAllocation = false;
inline bool reverseDirectory = false;
inline int failIterationAfter = -1;
inline std::set<std::string> openReaders;
inline size_t maxAllocation = 0;
}  // namespace test_storage

class HalFile {
  FILE* file_ = nullptr;
  std::string path_;
  std::vector<std::string> children_;
  size_t next_ = 0;
  bool directory_ = false, failed_ = false;

 public:
  HalFile() = default;
  explicit HalFile(std::string path, int flags = O_RDONLY) : path_(std::move(path)) {
    std::error_code ec;
    directory_ = std::filesystem::is_directory(path_, ec);
    if (directory_) {
      for (const auto& child : std::filesystem::directory_iterator(path_)) children_.push_back(child.path());
      std::sort(children_.begin(), children_.end());
      if (test_storage::reverseDirectory) std::reverse(children_.begin(), children_.end());
    } else {
      // Model SdFat: a path cannot be opened twice, or deleted/renamed while open.
      assert(!test_storage::openReaders.contains(path_));
      const int fd = ::open(path_.c_str(), flags, 0600);
      if (fd >= 0) file_ = ::fdopen(fd, (flags & O_RDWR) ? "r+b" : "rb");
      if (file_) test_storage::openReaders.insert(path_);
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
      next_ = other.next_;
      directory_ = std::exchange(other.directory_, false);
      failed_ = other.failed_;
    }
    return *this;
  }
  int read(void* dest, size_t count) { return file_ ? static_cast<int>(std::fread(dest, 1, count, file_)) : -1; }
  size_t write(const void* bytes, size_t count) {
    if (!file_ || (!test_storage::failWritePath.empty() && path_.ends_with(test_storage::failWritePath))) return 0;
    return std::fwrite(bytes, 1, count, file_);
  }
  bool sync() { return file_ && std::fflush(file_) == 0 && !test_storage::failSync; }
  bool seek(size_t offset) { return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0; }
  bool seek64(uint64_t offset) { return seek(static_cast<size_t>(offset)); }
  uint64_t fileSize64() const {
    std::error_code ec;
    return std::filesystem::file_size(path_, ec);
  }
  uint32_t modificationTime() const {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path_, ec);
    return ec ? 0 : static_cast<uint32_t>(stamp.time_since_epoch().count());
  }
  bool isDirectory() const { return directory_; }
  size_t getName(char* out, size_t size) const {
    const auto name = std::filesystem::path(path_).filename().string();
    std::snprintf(out, size, "%s", name.c_str());
    return name.size();
  }
  bool iterationFailed() const { return failed_; }
  HalFile openNextFile() {
    if (test_storage::failIterationAfter >= 0 && next_ == static_cast<size_t>(test_storage::failIterationAfter)) {
      failed_ = true;
      return {};
    }
    return next_ < children_.size() ? HalFile(children_[next_++]) : HalFile();
  }
  bool close() {
    bool ok = true;
    if (file_) {
      ok = std::fclose(file_) == 0 && !test_storage::failClose;
      file_ = nullptr;
      test_storage::openReaders.erase(path_);
    }
    directory_ = false;
    return ok;
  }
  explicit operator bool() const { return file_ || directory_; }
};
using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  HalFile open(const char* path, int flags = O_RDONLY) { return HalFile(test_storage::root + path, flags); }
  bool exists(const char* path) { return std::filesystem::exists(test_storage::root + path); }
  bool ensureDirectoryExists(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(test_storage::root + path, ec);
    return !ec;
  }
  bool remove(const char* path) {
    assert(!test_storage::openReaders.contains(test_storage::root + path));
    std::error_code ec;
    return std::filesystem::remove(test_storage::root + path, ec);
  }
  bool rename(const char* from, const char* to) {
    assert(!test_storage::openReaders.contains(test_storage::root + from));
    assert(!test_storage::openReaders.contains(test_storage::root + to));
    if (test_storage::failRenameFrom == from) {
      test_storage::failRenameFrom.clear();
      return false;
    }
    if (exists(to)) return false;
    std::error_code ec;
    std::filesystem::rename(test_storage::root + from, test_storage::root + to, ec);
    return !ec;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = open(path, O_RDWR | O_CREAT | O_TRUNC);
    return bool(file);
  }
};
#define Storage HalStorage::getInstance()
