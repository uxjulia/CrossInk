#!/usr/bin/env python3
"""Exercise Home's production TXT progress reader with packed index fixtures."""

from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/activities/home/RecentBookProgress.cpp").read_text()
start = source.index("float loadTxtProgressPercent(")
function = source[start:source.index("\n}", start) + 2]
constants = "\n".join(re.findall(r"constexpr uint(?:8|32)_t TXT_CACHE_.*?;", source))


def fixture(version=5, file_size=1000, pages=20, spacing=-3):
    # Serialized TXT v5: source size, spacing, five layout fields, alignment,
    # page count. Values deliberately differ so a shifted read cannot pass.
    return struct.pack("<IBIbiiiiiBI", 0x54585449, version, file_size, spacing,
                       480, 30, 7, 12, 16, 2, pages)


def vector(data):
    return "{" + ",".join(str(byte) for byte in data) + "}"


with tempfile.TemporaryDirectory(prefix="crossink-txt-progress-") as directory:
    directory = Path(directory)
    (directory / "production.inc").write_text(constants + "\n" + function)
    (directory / "test.cpp").write_text(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
struct RecentBook { std::string path = "/book.txt"; };
struct Txt {
  Txt(const std::string&, const char*) {}
  bool load() const { return true; }
  std::string getCachePath() const { return "/cache"; }
  uint32_t getFileSize() const { return 1000; }
};
int openFiles = 0;
struct FsFile {
  const std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  int read(void* out, size_t size) {
    size = std::min(size, bytes->size() - offset);
    std::memcpy(out, bytes->data() + offset, size);
    offset += size;
    return static_cast<int>(size);
  }
  void close() { assert(bytes); bytes = nullptr; --openFiles; }
};
struct {
  std::vector<uint8_t> progress = {4, 0, 0, 0, 0, 0};
  std::vector<uint8_t> index;
  bool openFileForRead(const char*, const std::string& path, FsFile& file) {
    // Match the hardware's single-reader restriction and check explicit closes.
    assert(openFiles == 0);
    file.bytes = path.ends_with("/index.bin") ? &index : &progress;
    ++openFiles;
    return true;
  }
} Storage;
namespace serialization {
template<class T> bool tryReadPod(FsFile& file, T& value) {
  return file.read(&value, sizeof(value)) == sizeof(value);
}
}
float clampProgressPercent(float value) { return std::clamp(value, 0.0f, 100.0f); }
#include "production.inc"
int main() {
''' + f"    Storage.index = {vector(fixture())};\n" + r'''
    assert(loadTxtProgressPercent(RecentBook{}) == 25.0f);
    assert(openFiles == 0);
    // Markdown uses the same TXT cache.
    assert(loadTxtProgressPercent(RecentBook{"/book.md"}) == 25.0f);
    const auto valid = Storage.index;
    for (size_t length = 0; length < valid.size(); ++length) {
      Storage.index.assign(valid.begin(), valid.begin() + length);
      assert(loadTxtProgressPercent(RecentBook{}) < 0);
      assert(openFiles == 0);
    }
''' + "\n".join(
        f"    Storage.index = {vector(data)};\n"
        "    assert(loadTxtProgressPercent(RecentBook{}) < 0);\n"
        "    assert(openFiles == 0);"
        for data in (fixture(version=4), fixture(file_size=999), fixture(pages=0))
    ) + r'''
    Storage.index = valid;
    Storage.progress.resize(3);
    assert(loadTxtProgressPercent(RecentBook{}) < 0);
    assert(openFiles == 0);
}
''')
    executable = directory / "test"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(directory / "test.cpp"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: TXT/Markdown progress, spacing header, stale/truncated indexes, explicit closes")
