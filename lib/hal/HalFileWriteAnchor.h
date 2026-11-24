#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hal_file_write_anchor {

inline constexpr size_t FIRST_SYNC_BYTES = 512;
inline constexpr size_t GROWTH_SYNC_BYTES = 32 * 1024;

// Only an empty writable file needs a new chain anchor. Existing clusters are
// already reachable even when a caller overwrites from the beginning.
inline bool required(const bool writable, const uint64_t initialSize) { return writable && initialSize == 0; }

template <typename File>
size_t writeGrowth(File& file, bool& failed, size_t& bytesSinceSync, const void* buffer, const size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  size_t totalWritten = 0;
  while (totalWritten < size) {
    const size_t room = GROWTH_SYNC_BYTES - bytesSinceSync;
    const size_t chunkSize = std::min(size - totalWritten, room);
    const size_t chunkWritten = file.write(bytes + totalWritten, chunkSize);
    if (chunkWritten != chunkSize) {
      const bool anchored = file.sync();
      if (anchored) {
        bytesSinceSync = 0;
      }
      failed = true;
      return totalWritten + chunkWritten;
    }

    totalWritten += chunkWritten;
    bytesSinceSync += chunkWritten;
    if (bytesSinceSync == GROWTH_SYNC_BYTES) {
      if (!file.sync()) {
        failed = true;
        return totalWritten - chunkWritten;
      }
      bytesSinceSync = 0;
    }
  }
  return totalWritten;
}

template <typename File>
size_t write(File& file, bool& pending, const bool growthEnabled, bool& failed, size_t& bytesSinceSync,
             const void* buffer, const size_t size) {
  if (failed) return 0;
  if (size == 0) return file.write(buffer, size);
  if (!pending) {
    if (!growthEnabled) return file.write(buffer, size);
    return writeGrowth(file, failed, bytesSinceSync, buffer, size);
  }

  const size_t firstSize = std::min(size, FIRST_SYNC_BYTES);
  const size_t firstWritten = file.write(buffer, firstSize);
  // Checkpoint even a short first write, since it may already have allocated a
  // cluster. Keep the failure latched so a later sync cannot mask it.
  const bool anchored = file.sync();
  if (firstWritten != firstSize || !anchored) {
    failed = true;
    return firstWritten == firstSize ? 0 : firstWritten;
  }

  pending = false;
  bytesSinceSync = 0;
  if (size == firstSize) return firstWritten;

  const auto* bytes = static_cast<const uint8_t*>(buffer);
  return firstWritten + writeGrowth(file, failed, bytesSinceSync, bytes + firstSize, size - firstSize);
}

}  // namespace hal_file_write_anchor
