#pragma once

#include <cstddef>
#include <cstdint>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t value) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t written = 0;
    for (; written < size; written++) {
      if (write(buffer[written]) != 1) break;
    }
    return written;
  }
};
