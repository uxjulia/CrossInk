#pragma once

#include <cstdint>

namespace MemoryBudget {
struct Snapshot {
  uint32_t freeHeap = UINT32_MAX;
  uint32_t maxAllocHeap = UINT32_MAX;
};
inline constexpr uint32_t EPUB_TEXT_LAYOUT_MIN_FREE = 0;
inline Snapshot snapshot() { return {}; }
inline bool hasHeapForEpubTextLayoutStart(Snapshot) { return true; }
inline bool hasHeap(Snapshot, uint32_t, uint32_t) { return true; }
inline bool shouldReleaseSdFontCachesForEpubInlineImage(Snapshot) { return false; }
inline bool hasHeapForEpubInlineImage(const char*, const char*) { return true; }
}  // namespace MemoryBudget
