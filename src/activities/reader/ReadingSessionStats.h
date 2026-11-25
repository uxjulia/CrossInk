#pragma once
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// One accepted interval awaiting post-refresh storage. The reader's render lock
// protects it; drain it before starting another interval or leaving the reader.
// Fixed storage avoids allocating on each page turn.
struct DeferredReadingStatsInterval {
  ReadingStatsDateTime end;
  uint32_t seconds = 0;
  uint8_t utcOffset = 255;
};

// Main-loop owned. Rendering and queued/held input keep periodic writes waiting;
// explicit lifecycle saves do not consult this delay.
class ReadingStatsSaveDelay {
  uint32_t lastBusyMs = 0;

 public:
  void defer(uint32_t now) { lastBusyMs = now; }
  bool ready(uint32_t now) const { return uint32_t(now - lastBusyMs) >= 1000; }
};

// Bounded, allocation-free dated totals for the live session. Capture each
// accepted page interval so pauses never become fictitious continuous reading.
class ReadingSessionDates {
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  std::array<uint8_t, READING_HISTORY_BYTES> historyBits{};
  uint32_t anchorDay = 0;
  ReadingStatsDate firstDate;
  ReadingStatsDateTime intervalStart;
  uint8_t intervalOffset = 255;

 public:
  void reset() { *this = {}; }
  void start(const ReadingStatsDateTime& local, uint8_t offset) {
    intervalStart = local;
    intervalOffset = offset;
  }
  void accept(const ReadingStatsDateTime& end, uint8_t offset, uint32_t seconds);
  void apply(BookReadingStats& book, GlobalReadingStats& global) const;
  const ReadingStatsDate& firstReadingDate() const { return firstDate; }
};

// Adds one finished tracking session to the book and global totals. Sessions of
// at least 60 seconds count as a session; at least 10 seconds add reading time.
// Dated totals are applied only after the same ten-second session floor.
void commitReadingSession(BookReadingStats& book, GlobalReadingStats& global, uint32_t seconds,
                          const ReadingSessionDates* dates);

// Copies the fields BookStatsActivity can edit from the on-disk copy, keeping
// the reader's live counters. The stats screen saves a preview that already
// includes the pending session, so replacing live stats would count it twice.
void importBookStatsEdits(BookReadingStats& live, const BookReadingStats& disk);

// Refreshes a reader's global stats after an overlay closes. Takes the disk
// copy only after a local reset; otherwise imports the completed-books count
// the stats screen can edit and keeps unsaved live counters.
void refreshGlobalStatsAfterOverlay(GlobalReadingStats& live, uint32_t resetRevisionAtOpen);

// Persist a preview that includes the live session without consuming it. Safe
// for temporary overlays, restart handoffs, and periodic durable checkpoints.
bool saveReadingSessionCheckpoint(const BookReadingStats& book, const GlobalReadingStats& global, uint32_t seconds,
                                  const ReadingSessionDates* dates, const std::string& cachePath);
