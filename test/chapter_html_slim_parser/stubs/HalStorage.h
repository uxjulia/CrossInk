#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

class HalFile : public Print {
  std::string contents_;
  size_t offset_ = 0;
  bool failNextRead_ = false;
  bool failNextSeek_ = false;
  bool open_ = false;

 public:
  void setContents(std::string contents) {
    contents_ = std::move(contents);
    offset_ = 0;
    open_ = true;
  }
  int available() const { return contents_.size() - offset_; }
  size_t position() const { return offset_; }
  size_t size() const { return contents_.size(); }
  int read(void* out, size_t count) {
    if (failNextRead_) {
      failNextRead_ = false;
      return -1;
    }
    count = std::min(count, contents_.size() - offset_);
    std::memcpy(out, contents_.data() + offset_, count);
    offset_ += count;
    return count;
  }
  bool seek(size_t offset) {
    if (failNextSeek_) {
      failNextSeek_ = false;
      return false;
    }
    if (!open_ || offset > contents_.size()) return false;
    offset_ = offset;
    return true;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  void failNextRead() { failNextRead_ = true; }
  void failNextSeek() { failNextSeek_ = true; }
  bool close() {
    open_ = false;
    return true;
  }
  bool isOpen() const { return open_; }
  operator bool() const { return open_; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  std::unordered_map<std::string, std::string> files;
  bool failNextSeekOnReadOpen = false;
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    auto it = files.find(path);
    if (it == files.end()) return false;
    file.setContents(it->second);
    if (failNextSeekOnReadOpen) {
      failNextSeekOnReadOpen = false;
      file.failNextSeek();
    }
    return true;
  }
  bool openFileForWrite(const char*, const std::string&, HalFile&) { return false; }
  bool remove(const char*) { return false; }
  bool exists(const char*) const { return false; }
  bool rename(const char*, const char*) { return false; }
};

#define Storage HalStorage::getInstance()
