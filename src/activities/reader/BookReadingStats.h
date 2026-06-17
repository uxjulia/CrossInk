#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"

// Per-book reading statistics, persisted to cachePath/stats_v5.bin.
struct BookReadingStats {
  uint16_t sessionCount = 0;              // Total times this book was opened
  uint32_t totalReadingSeconds = 0;       // Accumulated reading time in seconds
  uint32_t totalPagesTurned = 0;          // Total forward page turns after the dwell threshold
  bool isCompleted = false;               // Whether the user manually marked this book as finished
  uint16_t avgSecondsPerForwardPage = 0;  // Running average pace for time-left estimates
  uint16_t paceSampleCount = 0;           // Number of forward-page pace samples included in the average
  uint32_t estimatedTimeLeftSeconds = 0;  // Last live reader book time-left estimate; 0 means unavailable
  bool startDateManual = false;           // Permanent user override for the reading start date
  bool finishedDateManual = false;        // Permanent user override for the finished date
  ReadingStatsDate startDate;             // First qualifying reading date (or manual override)
  ReadingStatsDate finishedDate;          // Manual or auto-finished date on X3
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};

  // Loads stats from cachePath/stats_v5.bin, with fallback reads from the
  // previous versioned filename and legacy cachePath/stats.bin. Returns
  // default-constructed stats if no compatible file exists.
  static BookReadingStats load(const std::string& cachePath);

  // Saves stats to cachePath/stats_v5.bin.
  void save(const std::string& cachePath) const;

  // Deletes cachePath/stats_v5.bin, the previous versioned filename, and legacy
  // cachePath/stats.bin. Missing files are treated as success.
  static bool remove(const std::string& cachePath);

  // Updates the running reading pace with one forward page dwell sample.
  void recordForwardPageRead(uint32_t seconds);

  // Attributes reading time to the X3 local date/time buckets when RTC data exists.
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // Formats a duration in seconds into a human-readable string.
  // Output examples: "< 1 min", "45 min", "2h 30 min"
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
