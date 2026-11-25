#include <gtest/gtest.h>

#include "ReadingSessionStats.h"
namespace {
BookReadingStats savedBook;
GlobalReadingStats savedGlobal;
bool saveFails = false;
bool globalSaveFails = false;
}  // namespace
bool BookReadingStats::save(const std::string&) const {
  if (saveFails) return false;
  savedBook = *this;
  return true;
}
bool GlobalReadingStats::save() const {
  if (globalSaveFails) return false;
  savedGlobal = *this;
  return true;
}
bool GlobalReadingStats::loadForUpload(GlobalReadingStats& out) {
  out = savedGlobal;
  return true;
}
GlobalReadingStats GlobalReadingStats::load() { return savedGlobal; }
uint32_t GlobalReadingStats::localResetRevision() { return 0; }

TEST(ReadingStreak, AnchoredCalendarAllowsTodayToBeUnreadAndExpiresAfterAGap) {
  std::array<uint8_t, READING_HISTORY_BYTES> bits{};
  bits[0] = 14;  // yesterday and the two days before it
  const ReadingStatsDate today{2026, 10, 30};
  const auto anchor = readingStatsDayIndex(today);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor, bits, &today), 3);
  const ReadingStatsDate tomorrow{2026, 10, 31};
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor, bits, &tomorrow), 0);
  bits[0] = 15;
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor, bits, &today), 4);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor, bits, &tomorrow), 4);
}

TEST(ReadingStreak, IgnoresFutureBitsAndStopsAtTheWindowBoundary) {
  std::array<uint8_t, READING_HISTORY_BYTES> bits{};
  bits[0] = 13;  // future day, gap, today and yesterday
  const ReadingStatsDate today{2026, 10, 30};
  const auto anchor = readingStatsDayIndex(today);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor + 2, bits, &today), 2);
  bits.fill(255);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor, bits, &today), 730);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor + 729, bits, &today), 1);
  EXPECT_EQ(computeReadingHistoryCurrentStreak(anchor + 730, bits, &today), 0);
}

TEST(SessionCheckpoint, OverlayPreviewsNeverConsumeOrDoubleCountTheLiveSession) {
  BookReadingStats book;
  GlobalReadingStats global;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 30, nullptr, "/cache"));
  EXPECT_EQ(savedBook.totalReadingSeconds, 30u);
  EXPECT_EQ(savedGlobal.totalSessions, 0u);
  EXPECT_EQ(book.totalReadingSeconds, 0u);
  EXPECT_EQ(global.totalReadingSeconds, 0u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedBook.totalReadingSeconds, 60u);
  EXPECT_EQ(savedGlobal.totalSessions, 1u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 60u);
  EXPECT_EQ(savedGlobal.totalSessions, 1u);
  commitReadingSession(book, global, 60, nullptr);
  EXPECT_EQ(book.totalReadingSeconds, savedBook.totalReadingSeconds);
  EXPECT_EQ(global.totalSessions, savedGlobal.totalSessions);
}
TEST(SessionCheckpoint, ShortSegmentsKeepTheExistingWholeSessionThreshold) {
  BookReadingStats book;
  GlobalReadingStats global;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 6, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 0u);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 12, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 12u);
  EXPECT_EQ(global.totalReadingSeconds, 0u);
}
TEST(SessionCheckpoint, FailedBookWritesAreNotAcknowledged) {
  BookReadingStats book;
  GlobalReadingStats global;
  saveFails = true;
  EXPECT_FALSE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  saveFails = false;
}

namespace {
ReadingStatsDateTime at(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second = 0) {
  return {{year, month, day}, hour, minute, second};
}
void interval(ReadingSessionDates& dates, ReadingStatsDateTime start, uint32_t seconds, uint8_t offset = 48) {
  dates.start(start, offset);
  auto end = start;
  addSecondsToReadingStatsDateTime(end, seconds);
  dates.accept(end, offset, seconds);
}
uint16_t streak(const GlobalReadingStats& global, ReadingStatsDate today) {
  return computeReadingHistoryCurrentStreak(global.readingHistoryAnchorDay, global.readingHistoryBits, &today);
}
}  // namespace

TEST(SessionDates, PausesAcrossMidnightCreditTheActualDaysAndTimeBuckets) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 600);
  // The same reader stays open while paused for several hours.
  interval(dates, at(2026, 10, 23, 8, 0), 600);
  commitReadingSession(book, global, 1200, &dates);
  EXPECT_EQ(global.totalReadingSeconds, 1200u);
  EXPECT_EQ(global.totalSessions, 1u);
  EXPECT_EQ(streak(global, {2026, 10, 23}), 2u);
  EXPECT_EQ(global.longestReadingStreak, 2u);
  EXPECT_EQ(book.dayOfWeekSeconds[3], 600u);  // Thursday
  EXPECT_EQ(book.dayOfWeekSeconds[4], 600u);  // Friday
  EXPECT_EQ(book.timeOfDaySeconds[0], 600u);  // Morning
  EXPECT_EQ(book.timeOfDaySeconds[2], 600u);  // Evening
  EXPECT_EQ(book.dayOfWeekSeconds, global.dayOfWeekSeconds);
  EXPECT_EQ(book.timeOfDaySeconds, global.timeOfDaySeconds);
}

TEST(SessionDates, OnePageSpanningMidnightSplitsSecondsAtTheBoundary) {
  for (auto start : {at(2026, 12, 31, 23, 59, 55), at(2028, 2, 29, 23, 59, 55)}) {
    BookReadingStats book;
    GlobalReadingStats global;
    ReadingSessionDates dates;
    interval(dates, start, 10);
    auto end = start;
    addSecondsToReadingStatsDateTime(end, 10);
    commitReadingSession(book, global, 10, &dates);
    EXPECT_EQ(streak(global, end.date), 2u);
    EXPECT_EQ(global.dayOfWeekSeconds[readingStatsDayOfWeekIndex(start.date)], 5u);
    EXPECT_EQ(global.dayOfWeekSeconds[readingStatsDayOfWeekIndex(end.date)], 5u);
    EXPECT_EQ(global.timeOfDaySeconds[3], 10u);
  }
}

TEST(SessionDates, MultidayPausesDoNotManufactureReadingOnSkippedDays) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 60);
  interval(dates, at(2026, 10, 24, 8, 0), 60);
  commitReadingSession(book, global, 120, &dates);
  EXPECT_EQ(streak(global, {2026, 10, 24}), 1u);
  EXPECT_EQ(global.readingHistoryBits[0] & 7u, 5u);
  EXPECT_EQ(global.longestReadingStreak, 1u);
}

TEST(SessionDates, WholeSessionFloorRetainsShortIntervalsOnBothDates) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 23, 50), 6);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 6, &dates, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 0u);
  EXPECT_EQ(savedGlobal.readingHistoryAnchorDay, 0u);
  interval(dates, at(2026, 10, 23, 8, 0), 6);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 12, &dates, "/cache"));
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 12u);
  EXPECT_EQ(streak(savedGlobal, {2026, 10, 23}), 2u);
  EXPECT_EQ(savedGlobal.dayOfWeekSeconds[3], 6u);
  EXPECT_EQ(savedGlobal.dayOfWeekSeconds[4], 6u);
  // Preview again and then exit: each date's seconds must appear exactly once.
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 12, &dates, "/cache"));
  commitReadingSession(book, global, 12, &dates);
  EXPECT_EQ(global.readingHistoryBits, savedGlobal.readingHistoryBits);
  EXPECT_EQ(global.dayOfWeekSeconds, savedGlobal.dayOfWeekSeconds);
  EXPECT_EQ(global.totalReadingSeconds, 12u);
  dates.reset();
  interval(dates, at(2026, 10, 24, 9, 0), 10);
  commitReadingSession(book, global, 10, &dates);
  EXPECT_EQ(streak(global, {2026, 10, 24}), 3u);
  EXPECT_EQ(global.dayOfWeekSeconds[3], 6u);
  EXPECT_EQ(global.dayOfWeekSeconds[5], 10u);
}

TEST(SessionDates, ClockCorrectionsAndUnavailableDatesKeepOnlyUndatedTotals) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  dates.start(at(2026, 10, 22, 23, 0), 48);
  dates.accept(at(2026, 10, 23, 1, 0), 48, 60);  // Clock jumped forward.
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept(at(2026, 10, 22, 23, 0), 48, 60);  // Clock jumped back.
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept(at(2026, 10, 23, 1, 1), 49, 60);  // Offset changed.
  dates.start({}, 48);
  dates.accept(at(2026, 10, 23, 1, 1), 48, 60);
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept({}, 48, 60);
  interval(dates, at(2026, 10, 23, 1, 0), 60, 255);  // Invalid offset.
  commitReadingSession(book, global, 360, &dates);
  EXPECT_EQ(global.totalReadingSeconds, 360u);
  EXPECT_EQ(global.readingHistoryAnchorDay, 0u);
  EXPECT_EQ(global.timeOfDaySeconds, (std::array<uint32_t, 4>{}));
  EXPECT_FALSE(book.startDate.isValid());
  // A working clock later in the same session can provide dated reading.
  interval(dates, at(2026, 10, 24, 8, 0), 120);
  BookReadingStats laterBook;
  GlobalReadingStats laterGlobal;
  commitReadingSession(laterBook, laterGlobal, 480, &dates);
  EXPECT_EQ(laterGlobal.totalReadingSeconds, 480u);
  EXPECT_EQ(streak(laterGlobal, {2026, 10, 24}), 1u);
  EXPECT_EQ(laterBook.startDate.day, 24);
  EXPECT_EQ(laterGlobal.timeOfDaySeconds[0], 120u);
}

TEST(SessionDates, CheckpointsMergeExistingHistoryAndPreserveLongestStreak) {
  BookReadingStats book;
  GlobalReadingStats global;
  recordReadingSpanIntoHistory(global.readingHistoryAnchorDay, global.readingHistoryBits, at(2026, 10, 21, 12, 0), 60);
  global.longestReadingStreak = 35;
  book.startDate = {2026, 9, 1};
  book.startDateManual = true;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 60);
  interval(dates, at(2026, 10, 23, 8, 0), 60);
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 120, &dates, "/cache"));
  EXPECT_EQ(streak(savedGlobal, {2026, 10, 23}), 3u);
  EXPECT_EQ(savedGlobal.longestReadingStreak, 35u);
  EXPECT_EQ(savedBook.startDate.month, 9);
  EXPECT_EQ(savedBook.startDate.day, 1);
  EXPECT_EQ(global.readingHistoryAnchorDay, readingStatsDayIndex({2026, 10, 21}));
}

TEST(SessionDates, LocalOffsetsAndRtcRoundingFollowTheCapturedInterval) {
  for (uint8_t offset : {0, 28, 48, 70, 104}) {
    for (int delta : {-2, 0, 2}) {
      BookReadingStats book;
      GlobalReadingStats global;
      ReadingSessionDates dates;
      auto start = at(2026, 10, 23, 23, 59, 55);
      auto end = start;
      addSecondsToReadingStatsDateTime(end, 10 + delta);
      dates.start(start, offset);
      dates.accept(end, offset, 10);
      commitReadingSession(book, global, 10, &dates);
      EXPECT_EQ(streak(global, {2026, 10, 24}), 2u);
      EXPECT_EQ(global.dayOfWeekSeconds[4], 5u);
      EXPECT_EQ(global.dayOfWeekSeconds[5], 5u);
    }
  }
}

TEST(SessionCheckpoint, FailedGlobalWritesDoNotPassWithMatchingOldTotals) {
  BookReadingStats book;
  GlobalReadingStats global;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  // Only pages change: the previous verification by seconds/sessions would pass.
  global.totalPagesTurned = 7;
  globalSaveFails = true;
  EXPECT_FALSE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalPagesTurned, 0u);
  EXPECT_EQ(global.totalReadingSeconds, 0u);
  globalSaveFails = false;
  ASSERT_TRUE(saveReadingSessionCheckpoint(book, global, 60, nullptr, "/cache"));
  EXPECT_EQ(savedGlobal.totalPagesTurned, 7u);
  EXPECT_EQ(savedGlobal.totalReadingSeconds, 60u);
}
