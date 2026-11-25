#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>

constexpr int O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_APPEND = 4;
namespace battery_log_test {
inline std::map<std::string, std::string> files;
inline bool failRename = false;
inline bool failOpen = false;
inline unsigned writes = 0;
}  // namespace battery_log_test
class HalFile {
  std::string path_;
  size_t pos_ = 0;
  bool valid_;

 public:
  HalFile(const char* path, bool valid, bool append) : path_(path), valid_(valid) {
    if (valid && append) pos_ = battery_log_test::files[path_].size();
  }
  explicit operator bool() const { return valid_; }
  size_t size() const { return battery_log_test::files[path_].size(); }
  int read(char* out, size_t n) {
    const auto& text = battery_log_test::files[path_];
    n = std::min(n, text.size() - std::min(pos_, text.size()));
    std::memcpy(out, text.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }
  bool seekSet(size_t pos) {
    pos_ = pos;
    return true;
  }
  size_t write(const char* data, size_t n) {
    ++battery_log_test::writes;
    battery_log_test::files[path_].append(data, n);
    return n;
  }
  bool close() {
    valid_ = false;
    return true;
  }
};
struct StorageStub {
  bool exists(const char* path) const { return battery_log_test::files.count(path) != 0; }
  HalFile open(const char* path, int flags) const {
    const bool valid = !battery_log_test::failOpen && (exists(path) || (flags & O_CREAT));
    return HalFile(path, valid, flags & O_APPEND);
  }
  bool rename(const char* from, const char* to) {
    if (battery_log_test::failRename || exists(to) || !exists(from)) return false;
    battery_log_test::files[to] = battery_log_test::files[from];
    battery_log_test::files.erase(from);
    return true;
  }
};
inline StorageStub Storage;
