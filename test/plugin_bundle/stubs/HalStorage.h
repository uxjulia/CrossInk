#pragma once
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
namespace bundle_test {
inline std::filesystem::path root;
inline int renameCalls = 0, failRename = 0;
inline bool failJournalClose = false;
inline std::filesystem::path physical(const std::string& path) { return root / path.substr(1); }
}  // namespace bundle_test
class HalFile {
  std::string path;
  std::unique_ptr<std::fstream> stream;
  std::vector<std::string> children;
  size_t index = 0;
  bool opened = false, directory = false;

 public:
  HalFile() = default;
  explicit HalFile(const std::string& p, bool write = false) : path(p) {
    auto disk = bundle_test::physical(p);
    directory = std::filesystem::is_directory(disk);
    if (directory) {
      for (auto& e : std::filesystem::directory_iterator(disk))
        children.push_back(p + "/" + e.path().filename().string());
      opened = true;
    } else {
      stream = std::make_unique<std::fstream>(
          disk, std::ios::binary | (write ? std::ios::out | std::ios::trunc : std::ios::in));
      opened = stream->is_open();
    }
  }
  bool isDirectory() const { return directory; }
  bool isOpen() const { return opened; }
  explicit operator bool() const { return opened; }
  size_t size() const { return std::filesystem::file_size(bundle_test::physical(path)); }
  size_t getName(char* out, size_t capacity) const {
    auto name = std::filesystem::path(path).filename().string();
    if (name.size() >= capacity) return 0;
    memcpy(out, name.c_str(), name.size() + 1);
    return name.size();
  }
  HalFile openNextFile() { return index < children.size() ? HalFile(children[index++]) : HalFile(); }
  int read(void* data, size_t size) {
    stream->read(static_cast<char*>(data), size);
    return static_cast<int>(stream->gcount());
  }
  size_t write(const void* data, size_t size) {
    stream->write(static_cast<const char*>(data), size);
    return *stream ? size : 0;
  }
  bool close() {
    if (stream && stream->is_open()) stream->close();
    opened = false;
    return !(bundle_test::failJournalClose && path.ends_with("plugin-install.txt.part"));
  }
};
struct BundleStorage {
  bool exists(const char* p) { return std::filesystem::exists(bundle_test::physical(p)); }
  bool mkdir(const char* p) {
    auto physical = bundle_test::physical(p);
    return std::filesystem::is_directory(physical) || std::filesystem::create_directories(physical);
  }
  bool remove(const char* p) { return std::filesystem::remove(bundle_test::physical(p)); }
  bool removeDir(const char* p) { return std::filesystem::remove_all(bundle_test::physical(p)) > 0; }
  bool rename(const char* from, const char* to) {
    if (++bundle_test::renameCalls == bundle_test::failRename || !exists(from) || exists(to)) return false;
    std::error_code error;
    std::filesystem::rename(bundle_test::physical(from), bundle_test::physical(to), error);
    return !error;
  }
  HalFile open(const char* p) { return HalFile(p); }
  bool openFileForWrite(const char*, const std::string& p, HalFile& f) {
    f = HalFile(p, true);
    return bool(f);
  }
  bool readFileToString(const char*, const char* p, size_t cap, std::string& out) {
    if (!exists(p)) return false;
    std::ifstream file(bundle_test::physical(p));
    out.assign(std::istreambuf_iterator<char>(file), {});
    return out.size() <= cap;
  }
};
inline BundleStorage Storage;
