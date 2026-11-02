#pragma once
#include <cstddef>
#include <cstdio>
#include <utility>

inline size_t storageReadCalls = 0;
inline size_t storageReadBytes = 0;
inline size_t failStorageRead = 0;
inline size_t failStorageFrom = 0;
inline size_t injectedReadFailures = 0;
inline size_t openStorageFiles = 0;
class HalFile {
 public:
  std::FILE* file = nullptr;
  HalFile() = default;
  bool allocationFailed() const { return false; }
  explicit operator bool() const { return file != nullptr; }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) : file(std::exchange(other.file, nullptr)) {}
  HalFile& operator=(HalFile&& other) {
    close();
    file = std::exchange(other.file, nullptr);
    return *this;
  }
  ~HalFile() { close(); }
  void close() {
    if (file) {
      std::fclose(file);
      --openStorageFiles;
    }
    file = nullptr;
  }
  size_t size() {
    auto pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    auto len = std::ftell(file);
    std::fseek(file, pos, SEEK_SET);
    return len;
  }
  bool seekSet(size_t offset) { return file && std::fseek(file, offset, SEEK_SET) == 0; }
  int read(void* bytes, size_t count) {
    ++storageReadCalls;
    if (failStorageRead == storageReadCalls || (failStorageFrom && storageReadCalls >= failStorageFrom)) {
      ++injectedReadFailures;
      return 0;
    }
    const auto got = file ? std::fread(bytes, 1, count, file) : 0;
    storageReadBytes += got;
    return got;
  }
};
inline struct StorageStub {
  HalFile open(const char* path) {
    HalFile file;
    openFileForRead("TTF", path, file);
    return file;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.close();
    file.file = std::fopen(path, "rb");
    if (file.file) ++openStorageFiles;
    return file.file;
  }
} Storage;
