#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace dictionary_test {
inline std::map<std::string, std::string> files;
inline std::map<std::string, int> readers;
inline bool failRead = false;
inline unsigned duplicateOpens = 0;
}  // namespace dictionary_test
class HalFile {
 public:
  std::string path;
  size_t pos = 0;
  bool opened = false;
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  ~HalFile() { close(); }
  bool close() {
    if (opened) --dictionary_test::readers[path];
    opened = false;
    return true;
  }
  bool isOpen() const { return opened; }
  size_t fileSize() const { return opened ? dictionary_test::files.at(path).size() : 0; }
  size_t position() const { return pos; }
  bool available() const { return pos < fileSize(); }
  bool seekSet(size_t offset) {
    if (!opened || offset > fileSize()) return false;
    pos = offset;
    return true;
  }
  int read(void* out, size_t count) {
    if (!opened || dictionary_test::failRead) return -1;
    count = std::min(count, fileSize() - pos);
    std::memcpy(out, dictionary_test::files.at(path).data() + pos, count);
    pos += count;
    return static_cast<int>(count);
  }
  int read() {
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
  }
  int write(const void* data, size_t count) {
    if (!opened) return -1;
    auto& bytes = dictionary_test::files[path];
    if (pos + count > bytes.size()) bytes.resize(pos + count);
    std::memcpy(bytes.data() + pos, data, count);
    pos += count;
    return static_cast<int>(count);
  }
};
struct TestStorage {
  bool exists(const char* path) { return dictionary_test::files.count(path); }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.close();
    if (!exists(path)) return false;
    if (dictionary_test::readers[path]) {
      ++dictionary_test::duplicateOpens;
      return false;
    }
    file.path = path;
    file.pos = 0;
    file.opened = true;
    ++dictionary_test::readers[path];
    return true;
  }
  bool openFileForWrite(const char* tag, const char* path, HalFile& file) {
    dictionary_test::files[path].clear();
    return openFileForRead(tag, path, file);
  }
  bool openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }
  bool remove(const char* path) { return dictionary_test::files.erase(path); }
  bool rename(const char* from, const char* to) {
    if (!exists(from)) return false;
    dictionary_test::files[to] = std::move(dictionary_test::files[from]);
    return remove(from);
  }
};
inline TestStorage Storage;
