#pragma once
#include <cstdint>

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;         // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;   // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;      // Total page-turn actions across all books

  // Loads stats from /.crosspoint/global_stats.bin. Returns default-constructed
  // stats if the file is missing or the version byte does not match.
  static GlobalReadingStats load();

  // Saves stats to /.crosspoint/global_stats.bin.
  void save() const;
};
