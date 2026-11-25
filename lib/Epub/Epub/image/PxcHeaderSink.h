#pragma once

#include <Print.h>

// Capture the legacy PXC dimensions while forwarding its bytes to a cache file.
class PxcHeaderSink final : public Print {
 public:
  explicit PxcHeaderSink(Print& output) : output(output) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const size_t written = output.write(data, size);
    for (size_t i = 0; i < written && seen < sizeof(header); ++i) header[seen++] = data[i];
    return written;
  }
  uint16_t width() const { return seen < sizeof(header) ? 0 : uint16_t(header[0] | (header[1] << 8)); }
  uint16_t height() const { return seen < sizeof(header) ? 0 : uint16_t(header[2] | (header[3] << 8)); }

 private:
  Print& output;
  uint8_t header[4] = {};
  size_t seen = 0;
};
