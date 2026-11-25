#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

class HalFile {
 public:
  explicit HalFile(String content = {}) : content(std::move(content)) {}
  explicit HalFile(std::string* output, bool shortWrite, bool syncOk, bool closeOk)
      : output(output), shortWrite(shortWrite), syncOk(syncOk), closeOk(closeOk) {}
  int available() const {
    const size_t remaining = content.size() - std::min(position, content.size());
    return static_cast<int>(std::min(remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
  }
  int read(void* output, size_t length) {
    const size_t count = std::min(length, content.size() - position);
    memcpy(output, content.data() + position, count);
    position += count;
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* bytes, size_t length) {
    if (shortWrite && length) --length;
    if (output) output->append(reinterpret_cast<const char*>(bytes), length);
    return length;
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  bool sync() { return syncOk; }
  bool close() {
    content.clear();
    position = 0;
    output = nullptr;
    return closeOk;
  }

 private:
  std::string* output = nullptr;
  bool shortWrite = false, syncOk = true, closeOk = true;
  String content;
  size_t position = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  void reset() {
    files.clear();
    shortWrite = failSync = failClose = false;
    failRenameFrom.clear();
    writeAttempts = 0;
    failWrite = false;
    failBulkRead = false;
    bulkReadAttempts = 0;
    lastReadCap = 0;
    lastReadPath.clear();
  }

  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return files.erase(path) > 0; }

  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from) {
      failRenameFrom.clear();
      return false;
    }
    const auto source = files.find(from);
    if (source == files.end() || files.contains(to)) return false;
    files[to] = source->second;
    files.erase(from);
    return true;
  }

  bool writeFile(const char* path, const String& content) {
    ++writeAttempts;
    if (failWrite) {
      failWrite = false;
      files[path] = "";  // Simulate a failed write after truncation.
      return false;
    }
    files[path] = content.c_str();
    return true;
  }

  String readFile(const char* path) const {
    const auto file = files.find(path);
    return file == files.end() ? String() : String(file->second.c_str());
  }

  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    ++bulkReadAttempts;
    lastReadCap = cap;
    lastReadPath = path;
    out.clear();
    const auto file = files.find(path);
    if (file == files.end() || file->second.size() > cap) return false;
    out = file->second;
    // Return valid nonempty JSON even on failure, to ensure the caller honors
    // the HAL status rather than accidentally parsing a partial read.
    if (failBulkRead) {
      failBulkRead = false;
      return false;
    }
    return true;
  }
  void failNextBulkRead() { failBulkRead = true; }
  size_t bulkReadAttempts = 0;
  size_t lastReadCap = 0;
  std::string lastReadPath;

  void put(const std::string& path, std::string content) { files[path] = std::move(content); }
  void failNextRenameFrom(std::string path) { failRenameFrom = std::move(path); }
  void failNextWrite() { failWrite = true; }
  bool openFileForRead(const char*, const char* path, HalFile& file) const {
    if (!exists(path)) return false;
    file = HalFile(readFile(path));
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    if (!writeFile(path, "")) return false;
    file = HalFile(&files[path], shortWrite, !failSync, !failClose);
    return true;
  }
  size_t writeAttempts = 0;
  bool shortWrite = false, failSync = false, failClose = false;

 private:
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom;
  bool failWrite = false;
  bool failBulkRead = false;
};

inline HalStorage Storage;
