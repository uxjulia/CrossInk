#include "EpubLinkReturnState.h"

#include <HalStorage.h>
#include <Logging.h>
#include <OwnedFileSwap.h>

#include <algorithm>
#include <cstdint>

namespace EpubLinkReturnState {
std::optional<ReadingProgress> readingProgress(const Position (&positions)[MAX_DEPTH], const int depth,
                                               const bool transientPreview, const ReaderProgressSaveDebouncer& progress,
                                               const int lastSavedSpineIndex, const int lastSavedPageCount) {
  if (transientPreview && depth > 0) {
    const auto& origin = positions[resumeDepth(depth, true)];
    const uint32_t originKey =
        (static_cast<uint32_t>(origin.spineIndex) << 16) | static_cast<uint16_t>(origin.pageNumber);
    const int pageCount = progress.lastObservedPosition() == originKey
                              ? static_cast<int>(progress.lastObservedMetadata())
                              : (lastSavedSpineIndex == origin.spineIndex ? std::max(0, lastSavedPageCount) : 0);
    return ReadingProgress{origin.spineIndex, origin.pageNumber, pageCount};
  }
  if (!progress.hasPending()) return std::nullopt;
  const uint32_t position = progress.lastObservedPosition();
  return ReadingProgress{static_cast<int>(position >> 16), static_cast<int>(position & 0xffffU),
                         static_cast<int>(progress.lastObservedMetadata())};
}

void push(Position (&positions)[MAX_DEPTH], int& depth, const Position origin) {
  if (depth == MAX_DEPTH) {
    for (int i = 1; i < MAX_DEPTH; ++i) positions[i - 1] = positions[i];
    --depth;
  }
  positions[depth++] = origin;
}

int resumeDepth(const int depth, const bool transientPreview) {
  return transientPreview && depth > 0 ? depth - 1 : depth;
}

bool save(const std::string& cachePath, const Position (&positions)[MAX_DEPTH], const int depth,
          const bool transientPreview) {
  const std::string path = cachePath + "/links.bin";
  const int savedDepth = resumeDepth(depth, transientPreview);
  if (!OwnedFileSwap::recover(path)) return false;
  if (savedDepth == 0) return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  if (savedDepth < 0 || savedDepth > MAX_DEPTH) return false;

  uint8_t data[1 + MAX_DEPTH * 4];
  data[0] = static_cast<uint8_t>(savedDepth);
  for (int i = 0; i < savedDepth; ++i) {
    const auto& pos = positions[i];
    if (pos.spineIndex < 0 || pos.spineIndex > UINT16_MAX || pos.pageNumber < 0 || pos.pageNumber > UINT16_MAX) {
      LOG_ERR("ERS", "Link return position out of range");
      return false;
    }
    uint8_t* p = data + 1 + i * 4;
    p[0] = pos.spineIndex & 0xff;
    p[1] = (pos.spineIndex >> 8) & 0xff;
    p[2] = pos.pageNumber & 0xff;
    p[3] = (pos.pageNumber >> 8) & 0xff;
  }
  return OwnedFileSwap::save("ERS", path, data, 1 + savedDepth * 4);
}

bool load(const std::string& cachePath, Position (&positions)[MAX_DEPTH], int& depth, const int spineCount) {
  depth = 0;
  const std::string path = cachePath + "/links.bin";
  if (!OwnedFileSwap::recover(path)) return false;
  if (!Storage.exists(path.c_str())) return true;
  uint8_t data[1 + MAX_DEPTH * 4];
  FsFile file;
  bool valid = false;
  int loadedDepth = 0;
  if (Storage.openFileForRead("ERS", path, file)) {
    const size_t size = file.size();
    if (size >= 5 && size <= sizeof(data) && file.read(data, size) == static_cast<int>(size)) {
      loadedDepth = data[0];
      valid = loadedDepth >= 1 && loadedDepth <= MAX_DEPTH && size == static_cast<size_t>(1 + loadedDepth * 4);
      for (int i = 0; valid && i < loadedDepth; ++i) {
        const uint8_t* p = data + 1 + i * 4;
        valid = (p[0] | (p[1] << 8)) < spineCount;
      }
    }
    if (!file.close()) valid = false;
  }
  // Consume even malformed records, after closing the reader. A later clean
  // exit rewrites the current stack; a crash must not resurrect old entries.
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("ERS", "Failed to consume link return stack");
    return false;
  }
  if (!valid) {
    LOG_ERR("ERS", "Ignoring invalid link return stack");
    return false;
  }
  for (int i = 0; i < loadedDepth; ++i) {
    const uint8_t* p = data + 1 + i * 4;
    positions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
  }
  depth = loadedDepth;
  LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
  return true;
}
}  // namespace EpubLinkReturnState
