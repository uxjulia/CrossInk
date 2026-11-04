#pragma once
struct ReadingStatsDateTime {
  struct {
    unsigned year = 2026, month = 11, day = 2;
  } date;
  unsigned hour = 12, minute = 0;
};
inline bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime&) { return true; }
