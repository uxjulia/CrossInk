#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct CalibreUploadProgress {
  size_t received = 0;
  size_t total = 0;
  std::string filename;

  // The panel shares the SD bus on the X3/X4. Only byte-count changes wait.
  bool update(bool uploading, const std::string& name, size_t bytes, size_t size, uint32_t now) {
    if (!uploading) {
      if (!active) return false;
      *this = {};
      return true;
    }
    const bool newUpload = !active || name != filename || size != total || bytes < received;
    if (!newUpload && (bytes == received || now - lastRedraw < 4000)) return false;
    active = true;
    lastRedraw = now;
    received = bytes;
    total = size;
    filename = name;
    return true;
  }

 private:
  uint32_t lastRedraw = 0;
  bool active = false;
};
