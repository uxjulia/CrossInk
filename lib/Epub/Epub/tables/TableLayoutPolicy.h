#pragma once

#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

namespace TableLayoutPolicy {
// Select by SoC, not touch support, display profile, or transient free heap.
// C3 has no PSRAM; S3 keeps its existing rich-table headroom gate.
inline constexpr bool compactFirstTarget =
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
    true;
#else
    false;
#endif

constexpr bool useCompact(const bool compactFirst, const uint32_t freeBytes, const uint32_t largestBlockBytes) {
  return compactFirst || freeBytes < 96U * 1024U || largestBlockBytes < 56U * 1024U;
}
}  // namespace TableLayoutPolicy
