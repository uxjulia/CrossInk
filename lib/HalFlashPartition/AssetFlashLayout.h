#pragma once

#include <atomic>
#include <cstddef>

#if defined(SIMULATOR)
// The simulator's scalable-font profiles model PSRAM-equipped readers.
#if CROSSINK_SCALABLE_FONTS
#define CROSSINK_SHAPING_FLASH_CACHE 0
#else
#define CROSSINK_SHAPING_FLASH_CACHE 1
#endif
#elif defined(ESP_PLATFORM)
#include <BoardConfig.h>
#define CROSSINK_SHAPING_FLASH_CACHE FREEINK_MCU_C3
#else
#define CROSSINK_SHAPING_FLASH_CACHE 0
#endif

// Language slots retain their existing positions at the end of the partition.
// Indic shaping uses a reserved tail of hyphenation bank 1 on C3 devices.
namespace asset_flash_layout {
constexpr size_t LanguageBytes = 2 * 65536;
constexpr size_t ShapingBytes = 3 * 128 * 1024;
constexpr size_t shapingReserve(size_t hyphenationBytes) {
  return CROSSINK_SHAPING_FLASH_CACHE && hyphenationBytes >= 4 * ShapingBytes && hyphenationBytes % (2 * 65536) == 0
             ? ShapingBytes
             : 0;
}
// Confirm only the live hyphenation writer's actual reservation. Once a
// session begins without it, a later-free tail must wait until the next boot.
inline std::atomic<bool> shapingTailReserved{false};
inline void confirmShapingReservation(size_t reservedTail) {
  if (CROSSINK_SHAPING_FLASH_CACHE && reservedTail == ShapingBytes) shapingTailReserved.store(true);
}
inline bool hasShapingReservation() { return shapingTailReserved.load(); }
}  // namespace asset_flash_layout
