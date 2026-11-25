#pragma once
#include <array>
#include <cstdint>
inline constexpr unsigned READING_HISTORY_BYTES = 92;
struct GlobalReadingStats {
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint32_t readingHistoryAnchorDay = 0;
  uint16_t longestReadingStreak = 0;
  static bool saveServerStreak(const GlobalReadingStats&) { return true; }
};
