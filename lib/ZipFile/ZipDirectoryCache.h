#pragma once

#include <Memory.h>

#include <cstdint>
#include <string>

// Owned by one opened book, never shared globally across reader sessions.
// Only the ZIP directory is retained; entry names are always compared in full.
struct ZipDirectoryCache {
  HeapByteBuffer bytes;
  std::string path;
  size_t fileSize = 0;
  uint32_t modified = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
  uint16_t count = 0;
  bool attempted = false;

  void clear() {
    bytes.reset();
    path.clear();
    attempted = false;
  }
};
