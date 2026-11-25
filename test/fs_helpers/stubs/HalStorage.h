#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class HalFile {
 public:
  explicit operator bool() const { return open_; }
  bool isDirectory() const { return false; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return false; }
  HalFile openNextFile() { return {}; }
  void getName(char*, size_t) const {}
  void close() { open_ = false; }

  size_t fileSize64() const { return working.size(); }
  size_t position() const { return cursor; }
  size_t write(const uint8_t* data, size_t length) {
    if (!open_) return 0;
    ++writeCalls;
    const size_t written = writeCalls == shortWriteAt && length ? length - 1 : length;
    if (cursor + written > working.size()) working.resize(cursor + written);
    std::memcpy(working.data() + cursor, data, written);
    cursor += written;
    writeSizes.push_back(length);
    return written;
  }
  bool sync() {
    ++syncCalls;
    syncSnapshots.push_back(working);
    if (syncCalls == failSyncAt) return false;
    durable = working;
    return true;
  }

  bool open_ = false;
  size_t cursor = 0;
  size_t shortWriteAt = 0;
  size_t failSyncAt = 0;
  size_t writeCalls = 0;
  size_t syncCalls = 0;
  std::vector<size_t> writeSizes;
  std::vector<uint8_t> working;
  std::vector<uint8_t> durable;
  std::vector<std::vector<uint8_t>> syncSnapshots;
};

class HalStorage {
 public:
  HalFile open(const char*) { return {}; }
};

inline HalStorage Storage;
inline void yield() {}
