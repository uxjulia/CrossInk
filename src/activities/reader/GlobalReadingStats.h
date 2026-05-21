#pragma once
#include <cstdint>

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total page-turn actions across all books
  uint32_t completedBooks = 0;       // Books manually marked as finished

  // Loads stats from /.crosspoint/global_stats.bin. Returns default-constructed
  // stats if the file is missing or the version byte does not match.
  static GlobalReadingStats load();

  // Loads this device's local stats plus one synced stats file per other device
  // from /.crosspoint/synced_stats/. This device's own contribution file is
  // skipped to avoid double counting.
  static GlobalReadingStats loadAggregated();

  // Adds synced device stats to an already-loaded local stats snapshot. Use this
  // when the local stats may include in-memory changes that are not saved yet.
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);

  // Saves stats to /.crosspoint/global_stats.bin. If /.crosspoint/synced_stats/
  // already exists, also mirrors this device's contribution there.
  void save() const;
};
