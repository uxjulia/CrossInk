#pragma once

#include <cstdint>

// Admission for the pinned SDK's width-sized scratch and uint16_t bin counts.
// This optional quality path must never crowd out the decoder or cache stream.
namespace PngToneMapperPolicy {
// Grayscale replays and the standalone viewer must never independently choose
// a different quantizer as heap admission changes. Only cached BW decodes may
// use optional mapping; their exact levels are replayed from that cache.
inline bool mayMap(bool dithering, bool bwPass, bool caching) { return dithering && bwPass && caching; }

inline uint8_t blendWhite(uint8_t gray, uint8_t alpha) {
  return static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
}

inline bool admits(int srcWidth, int srcHeight, int dstWidth, int dstHeight, uint32_t freeHeap, uint32_t maxAlloc) {
  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0 || dstWidth > 65535 || dstWidth > srcWidth ||
      dstHeight > srcHeight || (dstWidth == srcWidth && dstHeight == srcHeight)) {
    return false;
  }
  // The largest integer-binned footprint is ceil(src/dst) on each axis.
  // More than 65535 samples would wrap SDK accCnt_ (and invalidate the mean).
  const uint64_t cols = (static_cast<uint64_t>(srcWidth) + dstWidth - 1) / dstWidth;
  const uint64_t rows = (static_cast<uint64_t>(srcHeight) + dstHeight - 1) / dstHeight;
  if (cols * rows > 65535) return false;
  constexpr uint64_t HEADROOM = 16U * 1024U;
  constexpr uint64_t ALLOCATOR_MARGIN = 512;
  const uint64_t total = 2ULL * srcWidth + 17ULL * dstWidth + 16;
  const uint64_t columns = 2ULL * srcWidth;
  const uint64_t errors = 4ULL * (dstWidth + 2);
  const uint64_t largest = columns > errors ? columns : errors;
  return total + HEADROOM + ALLOCATOR_MARGIN <= freeHeap && largest + ALLOCATOR_MARGIN <= maxAlloc;
}
}  // namespace PngToneMapperPolicy
