#pragma once

#include <HalStorage.h>

namespace serialization {
// Small ArduinoJson stream adapter: unescape directly into the JSON document
// without retaining an escaped response string or reading SD one byte at a time.
class JsonFileReader {
 public:
  explicit JsonFileReader(FsFile& file) : file(file) {}
  int read() {
    if (offset == size) {
      const int count = file.read(buffer, sizeof(buffer));
      if (count <= 0) {
        failed_ = file.position() < file.size();
        return -1;
      }
      size = static_cast<size_t>(count);
      offset = 0;
    }
    return buffer[offset++];
  }
  size_t readBytes(char* out, size_t count) {
    size_t copied = 0;
    for (; copied < count; ++copied) {
      const int value = read();
      if (value < 0) break;
      out[copied] = static_cast<char>(value);
    }
    return copied;
  }
  bool failed() const { return failed_; }

 private:
  FsFile& file;
  uint8_t buffer[128];
  size_t offset = 0;
  size_t size = 0;
  bool failed_ = false;
};
}  // namespace serialization
