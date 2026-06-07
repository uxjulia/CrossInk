#pragma once
#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total forward page turns after the dwell threshold
  uint32_t completedBooks = 0;       // Books manually marked as finished
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;

  static constexpr uint8_t CURRENT_FILE_VERSION = 3;
  static constexpr size_t CURRENT_FILE_SIZE = 159;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;

  // Loads stats from /.crosspoint/global_stats.bin. Returns default-constructed
  // stats if the file is missing or the version byte does not match.
  static GlobalReadingStats load();

  // Returns true when the optional synced stats directory exists.
  static bool hasSyncedStats();

  // Loads this device's local stats plus one synced stats file per other device
  // from /.crosspoint/synced_stats/. A stale file matching this device's MAC is
  // skipped to avoid double counting.
  static GlobalReadingStats loadAggregated();

  // Adds synced device stats to an already-loaded local stats snapshot. Use this
  // when the local stats may include in-memory changes that are not saved yet.
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);

  // Saves stats to /.crosspoint/global_stats.bin.
  void save() const;

  // Replaces /.crosspoint/global_stats.bin with a fresh empty file without
  // rotating or deleting any backup files.
  static bool resetLocal();

  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};
