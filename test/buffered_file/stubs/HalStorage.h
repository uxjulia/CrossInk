#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// In-memory file with independently injectable device I/O failures.
class HalFile {
 public:
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  size_t writeLimit = std::numeric_limits<size_t>::max();
  size_t readLimit = std::numeric_limits<size_t>::max();
  bool readError = false;
  bool seekError = false;
  size_t reads = 0;
  size_t writes = 0;
  size_t seeks = 0;

  int available() const { return static_cast<int>(bytes.size() - std::min(cursor, bytes.size())); }
  size_t position() const { return cursor; }
  int read(void* output, size_t length) {
    ++reads;
    if (readError) return -1;
    const size_t count = std::min({length, readLimit, bytes.size() - std::min(cursor, bytes.size())});
    if (count) std::memcpy(output, bytes.data() + cursor, count);
    cursor += count;
    return static_cast<int>(count);
  }
  size_t write(const void* input, size_t length) {
    ++writes;
    const size_t count = std::min(length, writeLimit);
    if (cursor + count > bytes.size()) bytes.resize(cursor + count);
    if (count) std::memcpy(bytes.data() + cursor, input, count);
    cursor += count;
    return count;
  }
  bool seek(size_t target) {
    ++seeks;
    if (seekError) return false;
    cursor = target;
    return true;
  }
};
using FsFile = HalFile;
