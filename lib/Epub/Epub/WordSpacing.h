#pragma once

#include <algorithm>
#include <cstdint>

namespace WordSpacing {
// Preserve saved 0..4 values. New byte values 5..8 represent -1..-4.
constexpr uint8_t MAX_VALUE = 8;
constexpr int MIN_LEVEL = -4;
constexpr int MAX_LEVEL = 4;
inline int level(const uint8_t value) { return value <= 4 ? value : -std::min<int>(value - 4, 4); }
inline uint8_t fromLevel(const int value) {
  const int bounded = std::clamp(value, MIN_LEVEL, MAX_LEVEL);
  return static_cast<uint8_t>(bounded < 0 ? 4 - bounded : bounded);
}
inline int sliderValue(const uint8_t value) { return level(value) - MIN_LEVEL; }
inline uint8_t fromSlider(const int value) { return fromLevel(value + MIN_LEVEL); }
inline int extra(const int naturalGap, const uint8_t value) {
  if (naturalGap <= 0) return 0;
  const int adjustment = level(value);
  // Keep the existing +10px steps. Negative steps remove 20% of the natural
  // gap each, retaining at least one pixel so words never overlap.
  return adjustment >= 0 ? adjustment * 10 : -std::min(naturalGap - 1, (naturalGap * -adjustment + 2) / 5);
}
}  // namespace WordSpacing
