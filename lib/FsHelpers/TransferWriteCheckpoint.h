#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

namespace FsHelpers {
// Commit each completed network buffer so an interrupted transfer leaves a
// recoverable prefix. HalFile handles anchoring the first sector of a new file.
inline bool writeAndCheckpoint(HalFile& file, const uint8_t* data, const size_t length) {
  if (!file) return false;
  if (length == 0) return true;
  if (!data) return false;

  const size_t written = file.write(data, length);
  const bool synced = file.sync();
  return written == length && synced;
}
}  // namespace FsHelpers
