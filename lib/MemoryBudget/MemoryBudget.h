#pragma once

#include <Arduino.h>
#include <Logging.h>

#include <cstdint>

namespace MemoryBudget {

struct HeapSnapshot {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};

constexpr uint32_t EPUB_INLINE_IMAGE_MIN_FREE = 72U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_MIN_MAX_ALLOC = 48U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE = 120U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC = 80U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_FREE = 96U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC = 48U * 1024U;
constexpr uint32_t IMAGE_DECODER_HEADROOM = 16U * 1024U;

inline HeapSnapshot snapshot() { return {ESP.getFreeHeap(), ESP.getMaxAllocHeap()}; }

inline bool hasHeap(const HeapSnapshot heap, const uint32_t minFree, const uint32_t minMaxAlloc) {
  return heap.freeHeap >= minFree && heap.maxAllocHeap >= minMaxAlloc;
}

inline bool shouldReleaseSdFontCachesForEpubInlineImage(const HeapSnapshot heap) {
  return !hasHeap(heap, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC);
}

inline bool hasHeapForEpubInlineImage(const char* tag, const char* source) {
  const auto heap = snapshot();
  if (hasHeap(heap, EPUB_INLINE_IMAGE_MIN_FREE, EPUB_INLINE_IMAGE_MIN_MAX_ALLOC)) {
    return true;
  }

  LOG_ERR(tag, "Low heap for inline image (%u free, %u max alloc, need %u/%u); suppressing %s", heap.freeHeap,
          heap.maxAllocHeap, EPUB_INLINE_IMAGE_MIN_FREE, EPUB_INLINE_IMAGE_MIN_MAX_ALLOC, source ? source : "");
  return false;
}

inline bool hasHeapForOptionalEpubRebuild(const char* tag, const char* action, const int spineIndex) {
  const auto heap = snapshot();
  if (hasHeap(heap, OPTIONAL_EPUB_REBUILD_MIN_FREE, OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC)) {
    return true;
  }

  LOG_DBG(tag, "Skipping %s for spine %d: low heap (free=%u, maxAlloc=%u, need free>=%u maxAlloc>=%u)", action,
          spineIndex, heap.freeHeap, heap.maxAllocHeap, OPTIONAL_EPUB_REBUILD_MIN_FREE,
          OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC);
  return false;
}

inline bool hasHeapForImageDecoder(const char* tag, const char* decoderName, const uint32_t decoderApproxBytes) {
  const auto heap = snapshot();
  const uint32_t minFree = decoderApproxBytes + IMAGE_DECODER_HEADROOM;
  if (hasHeap(heap, minFree, decoderApproxBytes)) {
    return true;
  }

  LOG_ERR(tag, "Not enough heap for %s decoder (%u free, %u max alloc, need %u/%u)", decoderName, heap.freeHeap,
          heap.maxAllocHeap, minFree, decoderApproxBytes);
  return false;
}

}  // namespace MemoryBudget
