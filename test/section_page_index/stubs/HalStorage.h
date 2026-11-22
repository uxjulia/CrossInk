#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

class HalFile : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  size_t failAt = std::numeric_limits<size_t>::max();
  size_t writeCalls = 0;
  size_t readCalls = 0;
  size_t seekCalls = 0;
  size_t failReadCall = std::numeric_limits<size_t>::max();

  size_t position() const { return cursor; }
  int available() const { return static_cast<int>(bytes.size() - std::min(cursor, bytes.size())); }

  size_t write(const uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* data, const size_t length) override {
    ++writeCalls;
    if (cursor >= failAt) return 0;
    const size_t writable = std::min(length, failAt - cursor);
    if (cursor + writable > bytes.size()) bytes.resize(cursor + writable);
    std::copy_n(data, writable, bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
    cursor += writable;
    return writable;
  }

  int read(void* data, const size_t length) {
    ++readCalls;
    if (readCalls == failReadCall) return 0;
    const size_t readable = std::min(length, bytes.size() - std::min(cursor, bytes.size()));
    std::copy_n(bytes.data() + cursor, readable, static_cast<uint8_t*>(data));
    cursor += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    ++seekCalls;
    if (position > bytes.size()) return false;
    cursor = position;
    return true;
  }
};

using FsFile = HalFile;
