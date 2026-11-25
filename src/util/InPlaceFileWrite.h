#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>

// Rewrites a small file's bytes without truncating it first. Opening for write
// truncates, which frees the file's cluster in both FAT copies and allocates it
// again on the first byte; overwriting in place keeps the cluster, so the save
// costs one data sector plus the directory entry (size and modified time).
//
// Not atomic by itself: callers either tolerate a torn write (a cache that is
// validated on read) or pair two slots so one always survives.
//
// padWithSpaces: for text whose readers skip trailing whitespace (JSON), a
// shorter save fills the rest of the file with spaces instead of recreating it.
inline bool writeFileInPlace(const char* moduleName, const char* path, const uint8_t* data, const size_t size,
                             const bool padWithSpaces = false) {
  HalFile file = Storage.open(path, O_RDWR | O_CREAT);
  if (!file) {
    LOG_ERR(moduleName, "Could not open %s for in-place write", path);
    return false;
  }
  const size_t pad = padWithSpaces && file.fileSize() > size ? file.fileSize() - size : 0;
  if (!padWithSpaces && file.fileSize() > size) {
    // Nothing shrinks in practice; a longer file would keep stale trailing bytes.
    file.close();
    if (!Storage.remove(path)) {
      LOG_ERR(moduleName, "Could not remove oversized %s", path);
      return false;
    }
    file = Storage.open(path, O_RDWR | O_CREAT);
    if (!file) {
      LOG_ERR(moduleName, "Could not recreate %s", path);
      return false;
    }
  }
  if (!file.seekSet(0)) {
    LOG_ERR(moduleName, "Could not seek %s", path);
    file.close();
    return false;
  }
  size_t written = file.write(data, size);
  static constexpr uint8_t spaces[32] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                         ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  for (size_t left = pad; written == size && left > 0;) {
    const size_t n = left < sizeof(spaces) ? left : sizeof(spaces);
    if (file.write(spaces, n) != n) written = 0;
    left -= n;
  }
  if (written != size) {
    LOG_ERR(moduleName, "Short in-place write to %s: %u/%u bytes", path, static_cast<unsigned>(written),
            static_cast<unsigned>(size));
    file.close();
    return false;
  }
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!synced || !closed) {
    LOG_ERR(moduleName, "Could not commit in-place write to %s", path);
    return false;
  }
  return true;
}
