#include "ReadingSessionStats.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>

void ReadingSessionDates::accept(const ReadingStatsDateTime& end, const uint8_t offset, const uint32_t seconds) {
  if (!isValidReadingStatsInterval(intervalStart, end, intervalOffset, offset, seconds)) return;
  if (!firstDate.isValid()) firstDate = intervalStart.date;
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, intervalStart, seconds);
  recordReadingSpanIntoHistory(anchorDay, historyBits, intervalStart, seconds);
}

void ReadingSessionDates::apply(BookReadingStats& book, GlobalReadingStats& global) const {
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    book.timeOfDaySeconds[i] += timeOfDaySeconds[i];
    global.timeOfDaySeconds[i] += timeOfDaySeconds[i];
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    book.dayOfWeekSeconds[i] += dayOfWeekSeconds[i];
    global.dayOfWeekSeconds[i] += dayOfWeekSeconds[i];
  }
  mergeReadingHistory(global.readingHistoryAnchorDay, global.readingHistoryBits, anchorDay, historyBits);
  global.longestReadingStreak =
      std::max(global.longestReadingStreak,
               computeReadingHistoryLongestStreak(global.readingHistoryAnchorDay, global.readingHistoryBits));
}

void commitReadingSession(BookReadingStats& book, GlobalReadingStats& global, const uint32_t seconds,
                          const ReadingSessionDates* dates) {
  // Page intervals longer than the idle threshold are rejected before they
  // reach the session total, so seconds is active reading time.
  if (seconds >= 60) {
    book.sessionCount++;
    global.totalSessions++;
  }
  if (seconds < 10) return;
  book.totalReadingSeconds += seconds;
  global.totalReadingSeconds += seconds;
  if (!dates) return;
  dates->apply(book, global);
  if (seconds >= 120 && !book.startDateManual && !book.startDate.isValid()) {
    book.startDate = dates->firstReadingDate();
  }
}

void importBookStatsEdits(BookReadingStats& live, const BookReadingStats& disk) {
  live.isCompleted = disk.isCompleted;
  live.startDateManual = disk.startDateManual;
  live.finishedDateManual = disk.finishedDateManual;
  live.startDate = disk.startDate;
  live.finishedDate = disk.finishedDate;
}

void refreshGlobalStatsAfterOverlay(GlobalReadingStats& live, const uint32_t resetRevisionAtOpen) {
  GlobalReadingStats disk = GlobalReadingStats::load();
  if (GlobalReadingStats::localResetRevision() != resetRevisionAtOpen) {
    live = disk;
  } else {
    live.completedBooks = disk.completedBooks;
  }
}

bool saveReadingSessionCheckpoint(const BookReadingStats& book, const GlobalReadingStats& global, uint32_t seconds,
                                  const ReadingSessionDates* dates, const std::string& cachePath) {
  // These two snapshots exceed the small task-stack budget. Allocate only at
  // checkpoint boundaries, never for every page or while rendering text.
  struct Preview {
    BookReadingStats book;
    GlobalReadingStats global;
  };
  auto preview = makeUniqueNoThrow<Preview>();
  if (!preview) {
    LOG_ERR("SessionStats", "Cannot allocate stats checkpoint");
    return false;
  }
  preview->book = book;
  preview->global = global;
  commitReadingSession(preview->book, preview->global, seconds, dates);
  if (!preview->book.save(cachePath)) return false;
  if (!preview->global.save()) return false;
  GlobalReadingStats checked;
  if (!GlobalReadingStats::loadForUpload(checked) ||
      checked.totalReadingSeconds != preview->global.totalReadingSeconds ||
      checked.totalSessions != preview->global.totalSessions) {
    LOG_ERR("SessionStats", "Global stats checkpoint could not be verified");
    return false;
  }
  return true;
}
