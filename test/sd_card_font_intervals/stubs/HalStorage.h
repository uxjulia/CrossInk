#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>

inline int failReadCall = -1, readCalls = 0, openFiles = 0;
inline int failSeekCall = -1, seekCalls = 0;
inline bool enforceSingleReader = false, failOpen = false;
inline unsigned long millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline uint32_t freeHeap = 1024 * 1024, maxAllocHeap = 1024 * 1024;
inline uint32_t (*freeHeapProbe)() = nullptr;
inline struct {
  uint32_t getFreeHeap() const { return freeHeapProbe ? freeHeapProbe() : freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
} ESP;
class HalFile {
  FILE* file_ = nullptr;

 public:
  ~HalFile() { close(); }
  bool open(const char* path) {
    close();
    if (failOpen || (enforceSingleReader && openFiles != 0)) return false;
    file_ = std::fopen(path, "rb");
    if (file_) ++openFiles;
    return file_;
  }
  void close() {
    if (file_) {
      std::fclose(file_);
      --openFiles;
    }
    file_ = nullptr;
  }
  size_t fileSize() const {
    if (!file_) return 0;
    const auto pos = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const auto size = std::ftell(file_);
    std::fseek(file_, pos, SEEK_SET);
    return size;
  }
  bool seekSet(uint32_t off) {
    if (seekCalls++ == failSeekCall) return false;
    return file_ && std::fseek(file_, off, SEEK_SET) == 0;
  }
  int read(void* dst, size_t n) {
    if (readCalls++ == failReadCall) return 0;
    return file_ ? static_cast<int>(std::fread(dst, 1, n, file_)) : 0;
  }
};
inline struct {
  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(path); }
} Storage;
