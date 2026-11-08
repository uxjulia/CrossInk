#include <CrossPointSettings.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include "DailyReadingStats.h"
#include "StatsUploadPayload.h"
using namespace DailyReadingStats;
namespace DailyReadingStats {
void resetForTest();
}
class DailyReading : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    resetForTest();
  }
};
static ReadingStatsDateTime dt(uint8_t day, uint8_t hour = 0, uint8_t minute = 0, uint8_t second = 0) {
  return {{2026, 10, day}, hour, minute, second};
}
static uint32_t dayOf(uint8_t day) { return readingStatsDayIndex(dt(day).date); }
static uint32_t secondsOn(uint8_t day) {
  Counter c;
  EXPECT_TRUE(flush());
  EXPECT_TRUE(read(dayOf(day), c));
  return c.seconds;
}
TEST_F(DailyReading, SplitsMidnightAndKeepsSeconds) {
  Session s;
  s.start(dt(1, 23, 59, 45), 48);
  EXPECT_TRUE(s.accept(dt(2, 0, 0, 15), 48, 30, 30));
  EXPECT_EQ(secondsOn(1), 15u);
  EXPECT_EQ(secondsOn(2), 15u);
  s.start(dt(2, 0, 1, 0), 48);
  EXPECT_TRUE(s.accept(dt(2, 0, 1, 11), 48, 11, 41));
  EXPECT_EQ(secondsOn(2), 26u);
}
TEST_F(DailyReading, PreservesSessionMinimumAndPauses) {
  Session s;
  s.start(dt(3, 12), 48);
  EXPECT_TRUE(s.accept(dt(3, 12, 0, 5), 48, 5, 5));
  EXPECT_EQ(secondsOn(3), 0u);
  // An overlay/idle boundary resumes at 13:00, never at the session's 12:00 start.
  s.start(dt(3, 13), 48);
  EXPECT_TRUE(s.accept(dt(3, 13, 0, 5), 48, 5, 10));
  EXPECT_EQ(secondsOn(3), 10u);
  Session shortSession;
  shortSession.start(dt(4, 12), 48);
  shortSession.accept(dt(4, 12, 0, 9), 48, 9, 9);
  shortSession.reset();
  EXPECT_EQ(secondsOn(4), 0u);
}
TEST_F(DailyReading, RejectsClockChangesAndUnavailableDates) {
  Session s;
  s.start({}, 48);
  EXPECT_TRUE(s.accept(dt(5, 12), 48, 60, 60));
  s.start(dt(5, 12), 48);
  EXPECT_TRUE(s.accept(dt(5, 13), 48, 60, 120));
  s.start(dt(5, 12), 48);
  EXPECT_TRUE(s.accept(dt(5, 12, 1), 49, 60, 180));
  s.start(dt(5, 12), 48);
  EXPECT_TRUE(s.accept({}, 48, 60, 240));
  EXPECT_EQ(secondsOn(5), 0u);
}
TEST_F(DailyReading, PendingShortIntervalsCanSpanNineDays) {
  Session s;
  for (uint8_t day = 6; day < 15; ++day) {
    s.start(dt(day, 12), 48);
    EXPECT_TRUE(s.accept(dt(day, 12, 0, 1), 48, 1, day - 5));
  }
  s.start(dt(15, 23, 59, 59), 48);
  EXPECT_TRUE(s.accept(dt(16, 0, 0, 1), 48, 2, 11));
  EXPECT_EQ(secondsOn(6), 1u);
  EXPECT_EQ(secondsOn(15), 1u);
  EXPECT_EQ(secondsOn(16), 1u);
}
TEST_F(DailyReading, DebouncesAndRetainsMonotonicAcknowledgements) {
  const uint32_t day = dayOf(17);
  EXPECT_TRUE(record(day, 11));
  Counter c;
  EXPECT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 0u);  // no page-turn write yet
  EXPECT_TRUE(flush());
  EXPECT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 11u);
  c.uploaded = c.seconds;
  EXPECT_TRUE(write(day, c));
  EXPECT_TRUE(record(day, 9));
  EXPECT_TRUE(flush());
  EXPECT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 20u);
  EXPECT_EQ(c.uploaded, 11u);
  EXPECT_FALSE(write(day, {19, 11}));
}
TEST_F(DailyReading, SecondsForIncludesUncheckpointedSeconds) {
  const uint32_t day = dayOf(28);
  uint32_t seconds = 99;
  EXPECT_TRUE(secondsFor(dayOf(29), seconds));
  EXPECT_EQ(seconds, 0u);  // missing day reads as zero
  EXPECT_TRUE(record(day, 7));
  EXPECT_TRUE(secondsFor(day, seconds));
  EXPECT_EQ(seconds, 7u);  // cached before the five-minute checkpoint
  EXPECT_TRUE(flush());
  EXPECT_TRUE(secondsFor(day, seconds));
  EXPECT_EQ(seconds, 7u);
}
TEST_F(DailyReading, SummarizesTodayAndSevenCalendarDays) {
  const auto day = [](uint8_t month, uint8_t d) { return readingStatsDayIndex({2026, month, d}); };
  EXPECT_TRUE(write(day(10, 31), {600, 0}));  // outside the window
  EXPECT_TRUE(write(day(11, 1), {700, 0}));
  EXPECT_TRUE(write(day(11, 4), {70, 0}));
  EXPECT_TRUE(record(day(11, 7), 30));  // cached, not yet checkpointed
  Summary summary = summarize(day(11, 7));
  EXPECT_TRUE(summary.hasToday);
  EXPECT_EQ(summary.todaySeconds, 30u);
  EXPECT_TRUE(summary.hasSevenDayAverage);
  EXPECT_EQ(summary.sevenDayAverageSeconds, 114u);  // (700 + 70 + 30) / 7, zero days included
  summary = summarize(day(11, 9));
  EXPECT_TRUE(summary.hasToday);
  EXPECT_EQ(summary.todaySeconds, 0u);
  EXPECT_EQ(summary.sevenDayAverageSeconds, 14u);  // (70 + 30) / 7

  ASSERT_TRUE(flush());
  for (const char* path : {"/.crosspoint/daily_reading/2026-11.a.bin", "/.crosspoint/daily_reading/2026-11.b.bin"}) {
    Storage.files[path]->bytes[20] ^= 1;
  }
  resetForTest();
  summary = summarize(day(11, 14));
  EXPECT_FALSE(summary.hasToday);
  EXPECT_FALSE(summary.hasSevenDayAverage);
}
TEST_F(DailyReading, LocalOffsetAndDateValidation) {
  ReadingStatsDateTime out;
  SETTINGS.clockUtcOffsetQ = 47;
  EXPECT_TRUE(getCurrentLocalDailyReadingDateTime(out));
  EXPECT_EQ(out.date.day, 30);
  EXPECT_EQ(out.date.month, 9);
  EXPECT_EQ(out.hour, 23);
  EXPECT_EQ(out.minute, 45);
  EXPECT_EQ(out.second, 10);
  SETTINGS.clockUtcOffsetQ = 255;
  EXPECT_FALSE(getCurrentLocalDailyReadingDateTime(out));
  SETTINGS.clockUtcOffsetQ = 48;
  halClock.valid = false;
  EXPECT_FALSE(getCurrentLocalDailyReadingDateTime(out));
  halClock.valid = true;
  EXPECT_FALSE((ReadingStatsDateTime{{2026, 2, 29}, 0, 0, 0}.isValid()));
  EXPECT_FALSE((ReadingStatsDateTime{{2026, 1, 1}, 24, 0, 0}.isValid()));
  EXPECT_TRUE((ReadingStatsDateTime{{2028, 2, 29}, 23, 59, 59}.isValid()));
}
TEST_F(DailyReading, BoundedPayloadWithOptionalDailySeconds) {
  char payload[StatsUploadPayload::CAPACITY];
  GlobalReadingStats g;
  g.totalReadingSeconds = 12345;
  ASSERT_TRUE(StatsUploadPayload::global(payload, sizeof(payload), "crossink-test", g, dayOf(21), 61));
  EXPECT_NE(std::string(payload).find("\"daily\":[{\"date\":\"2026-10-21\",\"seconds\":61,\"counter_id\":"),
            std::string::npos);
  ASSERT_TRUE(StatsUploadPayload::global(payload, sizeof(payload), "crossink-test", g));
  EXPECT_EQ(std::string(payload).find("daily"), std::string::npos);
  EXPECT_FALSE(StatsUploadPayload::global(payload, 20, "crossink-test", g, dayOf(21), 61));
}

TEST_F(DailyReading, HistoryFlagsNeverReplaceOrInventDailyTime) {
  GlobalReadingStats global;
  global.totalReadingSeconds = 7200;
  global.totalPagesTurned = 123;
  global.readingHistoryAnchorDay = dayOf(21);
  global.readingHistoryBits[0] = 7;  // Three read days, only one with a measured daily counter.
  ASSERT_TRUE(write(dayOf(20), {600, 0}));

  char payload[StatsUploadPayload::CAPACITY];
  ASSERT_TRUE(StatsUploadPayload::global(payload, sizeof(payload), "crossink-test", global));
  EXPECT_EQ(std::string(payload).find("\"daily\""), std::string::npos);
  EXPECT_NE(std::string(payload).find("\"seconds\":7200"), std::string::npos);
  EXPECT_NE(std::string(payload).find("\"pages\":123"), std::string::npos);

  unsigned uploads = 0;
  ASSERT_TRUE(uploadPending(
      [](void* context, uint32_t day, uint32_t seconds) {
        ++*static_cast<unsigned*>(context);
        EXPECT_EQ(day, dayOf(20));
        EXPECT_EQ(seconds, 600u);
        return true;
      },
      &uploads));
  EXPECT_EQ(uploads, 1u);
  Counter counter;
  ASSERT_TRUE(read(dayOf(20), counter));
  EXPECT_EQ(counter.seconds, 600u);
  EXPECT_EQ(counter.uploaded, 600u);
  // An overlapping bitmap-only snapshot after acknowledgment still sends no zero-time rows.
  ASSERT_TRUE(StatsUploadPayload::global(payload, sizeof(payload), "crossink-test", global));
  EXPECT_EQ(std::string(payload).find("\"daily\""), std::string::npos);
  EXPECT_EQ(secondsOn(20), 600u);
  EXPECT_EQ(secondsOn(19), 0u);
  EXPECT_EQ(secondsOn(21), 0u);
}

TEST_F(DailyReading, BookBatchMatchesSingleBookPayloadAndRollsBackOverflow) {
  BookReadingStats book;
  book.sessionCount = 3;
  book.totalReadingSeconds = 900;
  const char* first = "0123456789abcdef0123456789abcdef";
  const char* second = "fedcba9876543210fedcba9876543210";
  char single[StatsUploadPayload::CAPACITY];
  ASSERT_TRUE(StatsUploadPayload::book(single, sizeof(single), "crossink-test", first, book));

  char buffer[StatsUploadPayload::CAPACITY];
  StatsUploadPayload::BookBatch batch(buffer, sizeof(buffer));
  ASSERT_TRUE(batch.begin("crossink-test"));
  ASSERT_TRUE(batch.add(first, book));
  ASSERT_TRUE(batch.finish());
  EXPECT_STREQ(buffer, single);  // one item is byte-for-byte the single-book request

  ASSERT_TRUE(batch.begin("crossink-test"));
  ASSERT_TRUE(batch.add(first, book));
  ASSERT_TRUE(batch.add(second, book));
  EXPECT_FALSE(batch.add("not-a-document-id", book));
  size_t added = batch.count();
  while (batch.add(second, book)) ++added;  // fill until the buffer refuses an item
  EXPECT_EQ(batch.count(), added);
  ASSERT_TRUE(batch.finish());
  const std::string json(buffer);
  EXPECT_LE(json.size() + 1, sizeof(buffer));
  EXPECT_EQ(json.substr(json.size() - 3), "}]}");
  EXPECT_NE(json.find(std::string("},{\"document\":\"") + second), std::string::npos);
  size_t items = 0;
  for (size_t at = json.find("\"document\""); at != std::string::npos; at = json.find("\"document\"", at + 1)) ++items;
  EXPECT_EQ(items, batch.count());
  EXPECT_LE(batch.count(), StatsUploadPayload::BookBatch::MAX_ITEMS);
}

TEST_F(DailyReading, BookBatchCapsItemsPerRequest) {
  BookReadingStats book;
  static char buffer[16384];
  StatsUploadPayload::BookBatch batch(buffer, sizeof(buffer));
  ASSERT_TRUE(batch.begin("crossink-test"));
  for (size_t i = 0; i < StatsUploadPayload::BookBatch::MAX_ITEMS; ++i)
    ASSERT_TRUE(batch.add("0123456789abcdef0123456789abcdef", book));
  EXPECT_FALSE(batch.add("0123456789abcdef0123456789abcdef", book));
  EXPECT_TRUE(batch.finish());
}

TEST_F(DailyReading, KeepsDeferredShortSecondsAfterTransientStorageFault) {
  Session s;
  ASSERT_TRUE(record(dayOf(22), 11));
  ASSERT_TRUE(record(dayOf(23), 11));
  s.start(dt(24, 12), 48);
  ASSERT_TRUE(s.accept(dt(24, 12, 0, 5), 48, 5, 5));
  faults.failOpenWrite = true;  // Failure evicting the midnight cache slot before accepting the pending day.
  s.start(dt(24, 13), 48);
  EXPECT_FALSE(s.accept(dt(24, 13, 0, 5), 48, 5, 10));
  faults.failOpenWrite = false;
  s.start(dt(24, 14), 48);
  EXPECT_TRUE(s.accept(dt(24, 14, 0, 5), 48, 5, 15));
  EXPECT_EQ(secondsOn(24), 15u);
}
TEST_F(DailyReading, RecoveredUploadAckCanRetryAfterPublishFailure) {
  const uint32_t day = dayOf(25);
  ASSERT_TRUE(record(day, 60));
  ASSERT_TRUE(flush());
  Counter c;
  ASSERT_TRUE(read(day, c));
  c.uploaded = c.seconds;
  faults.failSync = true;
  EXPECT_FALSE(write(day, c));
  ASSERT_TRUE(record(day, 10));
  ASSERT_TRUE(flush());
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 70u);
  EXPECT_EQ(c.uploaded, 60u);
}

TEST_F(DailyReading, ReaderGateRejectsIdleAndStoppedSleepOrOverlayTimers) {
  uint32_t seconds = 0;
  EXPECT_TRUE(readingStatsIntervalSeconds(121000, 1000, 120, seconds));
  EXPECT_EQ(seconds, 120u);
  EXPECT_TRUE(readingStatsIntervalSeconds(121999, 1000, 120, seconds));
  EXPECT_EQ(seconds, 120u);
  EXPECT_FALSE(readingStatsIntervalSeconds(122000, 1000, 120, seconds));
  EXPECT_EQ(seconds, 0u);
  EXPECT_FALSE(readingStatsIntervalSeconds(100000, 0, 120, seconds));  // sleep/lock/overlay stop
  EXPECT_FALSE(readingStatsIntervalSeconds(1999, 1000, 120, seconds));
  EXPECT_TRUE(readingStatsIntervalSeconds(3999, UINT32_MAX - 1000, 120, seconds));
  EXPECT_EQ(seconds, 5u);
  EXPECT_TRUE(readingStatsIntervalSeconds(200500, 200000, 120, seconds) ==
              false);  // fresh resume starts a new interval
}

static std::string legacyFile(uint32_t day, const char* suffix = "") {
  char path[64];
  snprintf(path, sizeof(path), "%s/%05u.bin%s", DIRECTORY, day, suffix);
  return path;
}
static void seedLegacy(uint32_t day, Counter counter, const char* suffix = "") {
  Storage.mkdir(DIRECTORY);
  uint8_t bytes[13] = {1};
  for (int i = 0; i < 4; ++i) {
    bytes[1 + i] = counter.seconds >> (8 * i);
    bytes[5 + i] = counter.uploaded >> (8 * i);
  }
  uint32_t hash = 2166136261u ^ day;
  for (int i = 0; i < 9; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  for (int i = 0; i < 4; ++i) bytes[9 + i] = hash >> (8 * i);
  FsFile f;
  ASSERT_TRUE(Storage.openFileForWrite("test", legacyFile(day, suffix), f));
  ASSERT_EQ(f.write(bytes, sizeof(bytes)), sizeof(bytes));
  ASSERT_TRUE(f.close());
}
static std::string monthFile(const char* date, char slot) {
  return std::string(DIRECTORY) + "/" + date + "." + slot + ".bin";
}
static void reboot() {
  faults = {};
  resetForTest();
}
TEST_F(DailyReading, MonthUsesAtMostTwoFilesAndSkipsUnchangedWrites) {
  for (uint8_t day = 1; day <= 31; ++day) ASSERT_TRUE(write(dayOf(day), {day * 100u, 0}));
  ASSERT_EQ(Storage.files.size(), 3u);  // directory plus two fixed snapshots
  EXPECT_EQ(Storage.files.at(monthFile("2026-10", 'a'))->bytes.size(), 264u);
  EXPECT_EQ(Storage.files.at(monthFile("2026-10", 'b'))->bytes.size(), 264u);
  size_t writes = faults.writes;
  ASSERT_TRUE(write(dayOf(31), {3100, 0}));
  EXPECT_EQ(faults.writes, writes);
  reboot();
  for (uint8_t day = 1; day <= 31; ++day) {
    Counter c;
    ASSERT_TRUE(read(dayOf(day), c));
    EXPECT_EQ(c.seconds, day * 100u);
  }
}
TEST_F(DailyReading, EveryTornWriteFallsBackToPreviousSnapshot) {
  for (int cut = 0; cut < 264; ++cut) {
    SCOPED_TRACE(cut);
    Storage.reset();
    reboot();
    ASSERT_TRUE(write(dayOf(1), {100, 20}));
    faults.bytesUntilFailure = cut;
    EXPECT_FALSE(write(dayOf(1), {200, 100}));
    reboot();
    Counter c;
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 100u);
    EXPECT_EQ(c.uploaded, 20u);
    ASSERT_TRUE(write(dayOf(1), {200, 100}));
  }
}
TEST_F(DailyReading, MigrationSurvivesEveryInterruptedSnapshotWrite) {
  for (int cut = 0; cut < 528; ++cut) {
    SCOPED_TRACE(cut);
    Storage.reset();
    reboot();
    seedLegacy(dayOf(1), {100, 20});
    seedLegacy(dayOf(2), {200, 100}, ".tmp");
    faults.bytesUntilFailure = cut;
    Counter c;
    EXPECT_FALSE(read(dayOf(1), c));
    EXPECT_TRUE(Storage.exists(legacyFile(dayOf(1)).c_str()));
    EXPECT_TRUE(Storage.exists(legacyFile(dayOf(2), ".tmp").c_str()));
    reboot();
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 100u);
    EXPECT_EQ(c.uploaded, 20u);
    ASSERT_TRUE(read(dayOf(2), c));
    EXPECT_EQ(c.seconds, 200u);
    EXPECT_EQ(c.uploaded, 100u);
    EXPECT_FALSE(Storage.exists(legacyFile(dayOf(1)).c_str()));
    EXPECT_EQ(Storage.files.size(), 3u);
  }
}
TEST_F(DailyReading, MigrationMergesRecoveryCopiesAndResumesCleanup) {
  seedLegacy(dayOf(1), {100, 10});
  seedLegacy(dayOf(1), {120, 0}, ".bak");
  seedLegacy(dayOf(1), {110, 110}, ".tmp");
  seedLegacy(dayOf(31), {99, 0});
  Storage.failNextRemove(legacyFile(dayOf(1), ".bak"));
  Counter c;
  EXPECT_FALSE(read(dayOf(1), c));
  reboot();
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 120u);
  EXPECT_EQ(c.uploaded, 110u);
  // Either copy alone contains the complete migrated month.
  Storage.files.at(monthFile("2026-10", 'a'))->bytes[20] ^= 1;
  ASSERT_TRUE(read(dayOf(31), c));
  EXPECT_EQ(c.seconds, 99u);
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 120u);
}
TEST_F(DailyReading, MigrationRemovesCorruptSiblingsBeforeValidCopies) {
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    Storage.reset();
    reboot();
    seedLegacy(dayOf(1), {100, 20});
    seedLegacy(dayOf(1), {100, 20}, ".bak");
    seedLegacy(dayOf(1), {100, 20}, ".tmp");
    Storage.files.at(legacyFile(dayOf(1), ".tmp"))->bytes[9] ^= 1;
    Storage.failNextRemove(legacyFile(dayOf(1), suffix));
    Counter c;
    EXPECT_FALSE(read(dayOf(1), c));
    reboot();
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 100u);
    EXPECT_EQ(Storage.files.size(), 3u);
  }
}
TEST_F(DailyReading, PreservesUnknownAndUnrecoverableLegacyFormats) {
  seedLegacy(dayOf(1), {100, 20});
  Storage.files.at(legacyFile(dayOf(1)))->bytes[0] = 2;
  Counter c;
  EXPECT_FALSE(read(dayOf(1), c));
  EXPECT_FALSE(write(dayOf(1), {200, 20}));
  EXPECT_TRUE(Storage.exists(legacyFile(dayOf(1)).c_str()));
  Storage.files.at(legacyFile(dayOf(1)))->bytes[0] = 1;
  Storage.files.at(legacyFile(dayOf(1)))->bytes[9] ^= 1;
  EXPECT_FALSE(read(dayOf(1), c));
  EXPECT_FALSE(write(dayOf(1), {200, 20}));
}
TEST_F(DailyReading, MonthlyCorruptionAndFutureVersionsNeverResetHistory) {
  ASSERT_TRUE(write(dayOf(1), {100, 10}));
  ASSERT_TRUE(write(dayOf(1), {200, 20}));
  auto& a = Storage.files.at(monthFile("2026-10", 'a'))->bytes;
  auto& b = Storage.files.at(monthFile("2026-10", 'b'))->bytes;
  b[20] ^= 1;
  Counter c;
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 100u);
  a[20] ^= 1;
  EXPECT_FALSE(read(dayOf(1), c));
  EXPECT_FALSE(write(dayOf(1), {300, 0}));
  a[20] ^= 1;
  b[0] = 3;
  EXPECT_FALSE(read(dayOf(1), c));
  EXPECT_FALSE(write(dayOf(1), {300, 0}));
}
TEST_F(DailyReading, StorageFailuresPreserveExistingCheckpoint) {
  ASSERT_TRUE(write(dayOf(1), {100, 10}));
  faults.failOpenWrite = true;
  EXPECT_FALSE(write(dayOf(1), {200, 20}));
  reboot();
  faults.failReadPath = monthFile("2026-10", 'a');
  EXPECT_FALSE(write(dayOf(1), {200, 20}));
  reboot();
  faults.failSync = true;
  EXPECT_FALSE(write(dayOf(1), {200, 20}));
  reboot();
  Counter c;
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_GE(c.seconds, 100u);
  faults.failScan = true;
  resetForTest();
  EXPECT_FALSE(uploadPending([](void*, uint32_t, uint32_t) { return true; }, nullptr));
  reboot();
  ASSERT_TRUE(read(dayOf(1), c));
}
TEST_F(DailyReading, RetainsYearsOfflineAndAcknowledgesOnlyAcceptedDays) {
  std::map<uint32_t, uint32_t> expected;
  for (uint16_t year : {2000, 2024, 2026, 2028, 2099}) {
    for (uint8_t month : {1, 2, 12}) {
      const uint32_t day = readingStatsDayIndex({year, month, daysInMonth(year, month)});
      ASSERT_TRUE(write(day, {100, 0}));
      expected[day] = 100;
    }
  }
  std::map<uint32_t, uint32_t> received;
  auto accept = [](void* ctx, uint32_t day, uint32_t seconds) {
    (*static_cast<std::map<uint32_t, uint32_t>*>(ctx))[day] = seconds;
    return true;
  };
  EXPECT_FALSE(uploadPending([](void*, uint32_t, uint32_t) { return false; }, nullptr));
  ASSERT_TRUE(uploadPending(accept, &received));
  EXPECT_EQ(received, expected);
  received.clear();
  ASSERT_TRUE(uploadPending(accept, &received));
  EXPECT_TRUE(received.empty());
  reboot();
  ASSERT_TRUE(uploadPending(accept, &received));
  EXPECT_TRUE(received.empty());
}
TEST_F(DailyReading, InterruptedAcknowledgmentRetriesCumulativeTotal) {
  ASSERT_TRUE(write(dayOf(1), {100, 0}));
  EXPECT_FALSE(uploadPending(
      [](void*, uint32_t, uint32_t) {
        faults.bytesUntilFailure = 20;
        return true;
      },
      nullptr));
  reboot();
  Counter c;
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.uploaded, 0u);
  uint32_t total = 0;
  ASSERT_TRUE(uploadPending(
      [](void* ctx, uint32_t, uint32_t seconds) {
        *static_cast<uint32_t*>(ctx) = seconds;
        return true;
      },
      &total));
  EXPECT_EQ(total, 100u);
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.uploaded, 100u);
}

TEST_F(DailyReading, FirstEverCheckpointCanRetryAfterEveryInterruption) {
  for (int cut = 0; cut < 528; ++cut) {
    SCOPED_TRACE(cut);
    Storage.reset();
    reboot();
    faults.bytesUntilFailure = cut;
    EXPECT_FALSE(write(dayOf(1), {100, 0}));
    reboot();
    Counter c;
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 0u);
    ASSERT_TRUE(write(dayOf(1), {100, 0}));
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 100u);
  }
}

static void snapshotWord(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[offset + i] = value >> (8 * i);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < 260; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  for (int i = 0; i < 4; ++i) bytes[260 + i] = hash >> (8 * i);
}
TEST_F(DailyReading, GenerationWrapSelectsNewestSnapshot) {
  ASSERT_TRUE(write(dayOf(1), {100, 0}));
  ASSERT_TRUE(write(dayOf(1), {200, 100}));
  snapshotWord(Storage.files.at(monthFile("2026-10", 'a'))->bytes, 8, UINT32_MAX);
  snapshotWord(Storage.files.at(monthFile("2026-10", 'b'))->bytes, 8, 0);
  Counter c;
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 200u);
  ASSERT_TRUE(write(dayOf(1), {300, 200}));
  reboot();
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 300u);
}
TEST_F(DailyReading, MonthIdentityAndImpossibleRecordsAreValidated) {
  const auto day = readingStatsDayIndex({2028, 2, 29});
  ASSERT_TRUE(write(day, {100, 0}));
  ASSERT_TRUE(write(day, {200, 100}));
  auto& b = Storage.files.at(monthFile("2028-02", 'b'))->bytes;
  const auto original = b;
  snapshotWord(b, 4, day);  // not February 1
  Counter c;
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 100u);
  b = original;
  snapshotWord(b, 12 + 29 * 8, 1);  // February 30
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 100u);
  b = original;
  snapshotWord(b, 12 + 28 * 8 + 4, 201);  // acknowledgment above total
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 100u);
}
TEST_F(DailyReading, BackwardDatesAndMonthBoundariesPreserveEarlierTotals) {
  const auto jan = readingStatsDayIndex({2028, 1, 31});
  const auto feb = readingStatsDayIndex({2028, 2, 1});
  const auto leap = readingStatsDayIndex({2028, 2, 29});
  const auto march = readingStatsDayIndex({2028, 3, 1});
  ASSERT_TRUE(record(march, 60));
  ASSERT_TRUE(record(leap, 60));
  ASSERT_TRUE(record(jan, 60));
  ASSERT_TRUE(record(feb, 60));
  ASSERT_TRUE(flush());
  reboot();
  for (auto day : {jan, feb, leap, march}) {
    Counter c;
    ASSERT_TRUE(read(day, c));
    EXPECT_EQ(c.seconds, 60u);
  }
  Counter c;
  EXPECT_FALSE(read(UINT32_MAX, c));
  EXPECT_FALSE(write(UINT32_MAX, {100, 0}));
}

TEST_F(DailyReading, DamagedLegacyMonthDoesNotBlockUnrelatedReading) {
  const uint32_t oldDay = readingStatsDayIndex({2025, 1, 1});
  seedLegacy(oldDay, {100, 20}, ".tmp");
  Storage.files.at(legacyFile(oldDay, ".tmp"))->bytes.resize(3);
  Counter c;
  EXPECT_FALSE(read(oldDay, c));
  ASSERT_TRUE(record(dayOf(1), 60));
  ASSERT_TRUE(flush());
  reboot();
  ASSERT_TRUE(read(dayOf(1), c));
  EXPECT_EQ(c.seconds, 60u);
  EXPECT_TRUE(Storage.exists(legacyFile(oldDay, ".tmp").c_str()));
  EXPECT_FALSE(uploadPending([](void*, uint32_t, uint32_t) { return true; }, nullptr));
}
TEST_F(DailyReading, ReadErrorsNeverOverwriteTheNewerSnapshot) {
  for (bool shortRead : {false, true}) {
    for (size_t offset : {0u, 12u, 20u, 260u}) {
      SCOPED_TRACE(offset);
      Storage.reset();
      reboot();
      ASSERT_TRUE(write(dayOf(1), {100, 10}));
      ASSERT_TRUE(write(dayOf(1), {200, 100}));
      const auto original = Storage.files.at(monthFile("2026-10", 'b'))->bytes;
      const size_t writes = faults.writes;
      faults.readErrorPath = monthFile("2026-10", 'b');
      faults.readErrorAfter = offset;
      faults.shortRead = shortRead;
      Counter c;
      EXPECT_FALSE(read(dayOf(1), c));
      EXPECT_FALSE(write(dayOf(2), {60, 0}));
      EXPECT_EQ(faults.writes, writes);
      EXPECT_EQ(Storage.files.at(monthFile("2026-10", 'b'))->bytes, original);
      reboot();
      ASSERT_TRUE(read(dayOf(1), c));
      EXPECT_EQ(c.seconds, 200u);
      EXPECT_EQ(c.uploaded, 100u);
    }
  }
}
TEST_F(DailyReading, LegacyReadErrorsKeepAllSourcesAndHigherTotals) {
  for (bool shortRead : {false, true}) {
    Storage.reset();
    reboot();
    seedLegacy(dayOf(1), {100, 10});
    seedLegacy(dayOf(1), {200, 100}, ".bak");
    faults.readErrorPath = legacyFile(dayOf(1), ".bak");
    faults.shortRead = shortRead;
    const size_t writes = faults.writes;
    Counter c;
    EXPECT_FALSE(read(dayOf(1), c));
    EXPECT_EQ(faults.writes, writes);
    EXPECT_TRUE(Storage.exists(legacyFile(dayOf(1)).c_str()));
    EXPECT_TRUE(Storage.exists(legacyFile(dayOf(1), ".bak").c_str()));
    reboot();
    ASSERT_TRUE(read(dayOf(1), c));
    EXPECT_EQ(c.seconds, 200u);
    EXPECT_EQ(c.uploaded, 100u);
  }
}

static uint32_t dateDay(uint16_t year, uint8_t month, uint8_t day = 1) {
  return readingStatsDayIndex({year, month, day});
}
static bool hasMonth(const char* month) {
  return Storage.exists(monthFile(month, 'a').c_str()) || Storage.exists(monthFile(month, 'b').c_str());
}
TEST_F(DailyReading, CleanupKeepsRecentFutureAndUnsyncedMonths) {
  ASSERT_TRUE(write(dateDay(2026, 5), {100, 100}));
  ASSERT_TRUE(write(dateDay(2026, 6), {100, 0}));
  ASSERT_TRUE(write(dateDay(2026, 7), {100, 99}));
  ASSERT_TRUE(write(dateDay(2026, 8), {100, 100}));
  ASSERT_TRUE(write(dateDay(2026, 8, 31), {1, 0}));
  ASSERT_TRUE(write(dateDay(2026, 9), {100, 100}));
  ASSERT_TRUE(write(dateDay(2026, 10), {100, 100}));
  ASSERT_TRUE(write(dateDay(2026, 11), {100, 100}));
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_FALSE(hasMonth("2026-05"));
  for (const char* month : {"2026-06", "2026-07", "2026-08", "2026-09", "2026-10", "2026-11"})
    EXPECT_TRUE(hasMonth(month));
  ASSERT_TRUE(uploadPending([](void*, uint32_t, uint32_t) { return true; }, nullptr));
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  for (const char* month : {"2026-06", "2026-07", "2026-08"}) EXPECT_FALSE(hasMonth(month));
  for (const char* month : {"2026-09", "2026-10", "2026-11"}) EXPECT_TRUE(hasMonth(month));
}
TEST_F(DailyReading, CleanupUsesCalendarMonthsAcrossYearAndLeapBoundaries) {
  ASSERT_TRUE(write(dateDay(2027, 11, 30), {100, 100}));
  ASSERT_TRUE(write(dateDay(2027, 12, 31), {100, 100}));
  ASSERT_TRUE(write(dateDay(2028, 1), {100, 100}));
  ASSERT_TRUE(pruneSyncedMonths({2028, 1, 1}));
  EXPECT_FALSE(hasMonth("2027-11"));
  EXPECT_TRUE(hasMonth("2027-12"));
  EXPECT_TRUE(hasMonth("2028-01"));
  ASSERT_TRUE(write(dateDay(2028, 2, 29), {100, 100}));
  ASSERT_TRUE(pruneSyncedMonths({2028, 3, 1}));
  EXPECT_FALSE(hasMonth("2027-12"));
  EXPECT_FALSE(hasMonth("2028-01"));
  EXPECT_TRUE(hasMonth("2028-02"));
}
TEST_F(DailyReading, CleanupWithoutValidDateChangesNothing) {
  ASSERT_TRUE(write(dateDay(2000, 1), {100, 100}));
  const size_t writes = faults.writes;
  for (ReadingStatsDate date : {ReadingStatsDate{}, {2026, 2, 29}, {1999, 12, 31}, {2100, 1, 1}}) {
    EXPECT_FALSE(pruneSyncedMonths(date));
    EXPECT_TRUE(hasMonth("2000-01"));
  }
  EXPECT_TRUE(pruneSyncedMonths({2000, 1, 1}));
  EXPECT_TRUE(pruneSyncedMonths({2000, 2, 29}));
  EXPECT_TRUE(hasMonth("2000-01"));
  EXPECT_EQ(faults.writes, writes);
}
TEST_F(DailyReading, CleanupFlushesUnsyncedCachedSecondsAndClearsDeletedCache) {
  const uint32_t day = dateDay(2026, 8);
  ASSERT_TRUE(record(day, 60));
  ASSERT_TRUE(write(day, {60, 60}));
  ASSERT_TRUE(record(day, 5));  // accepted, but not checkpointed
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  Counter c;
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 65u);
  EXPECT_EQ(c.uploaded, 60u);
  ASSERT_TRUE(write(day, {65, 65}));
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_FALSE(hasMonth("2026-08"));
  uint32_t seconds;
  ASSERT_TRUE(secondsFor(day, seconds));
  EXPECT_EQ(seconds, 0u);
  ASSERT_TRUE(flush());
  EXPECT_FALSE(hasMonth("2026-08"));
}
TEST_F(DailyReading, FailedFlushPreventsCleanup) {
  ASSERT_TRUE(write(dateDay(2026, 7), {100, 100}));
  ASSERT_TRUE(record(dateDay(2026, 8), 5));
  faults.failOpenWrite = true;
  EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_TRUE(hasMonth("2026-07"));
  faults.failOpenWrite = false;
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_FALSE(hasMonth("2026-07"));
  EXPECT_TRUE(hasMonth("2026-08"));
}
TEST_F(DailyReading, CleanupRetriesEitherDeletionWithoutLosingNewestAcknowledgment) {
  for (bool newestIsA : {false, true}) {
    for (bool failNewest : {false, true}) {
      Storage.reset();
      reboot();
      const uint32_t day = dateDay(2026, 8);
      ASSERT_TRUE(write(day, {100, 0}));
      ASSERT_TRUE(write(day, {100, 100}));
      if (newestIsA) ASSERT_TRUE(write(day, {200, 200}));
      const char newest = newestIsA ? 'a' : 'b';
      const char older = newestIsA ? 'b' : 'a';
      Storage.failNextRemove(monthFile("2026-08", failNewest ? newest : older));
      EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
      EXPECT_TRUE(Storage.exists(monthFile("2026-08", newest).c_str()));
      EXPECT_EQ(Storage.exists(monthFile("2026-08", older).c_str()), !failNewest);
      reboot();
      Counter c;
      ASSERT_TRUE(read(day, c));
      EXPECT_EQ(c.seconds, newestIsA ? 200u : 100u);
      EXPECT_EQ(c.uploaded, c.seconds);
      ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
      EXPECT_FALSE(hasMonth("2026-08"));
    }
  }
}
TEST_F(DailyReading, CleanupPreservesCorruptOrUnknownSnapshotsAndContinuesOtherMonths) {
  for (bool futureVersion : {false, true}) {
    Storage.reset();
    reboot();
    const uint32_t day = dateDay(2026, 7);
    ASSERT_TRUE(write(day, {100, 100}));
    ASSERT_TRUE(write(day, {200, 0}));
    auto& bytes = Storage.files.at(monthFile("2026-07", 'b'))->bytes;
    if (futureVersion)
      bytes[0] = 3;
    else
      bytes[20] ^= 1;
    ASSERT_TRUE(write(dateDay(2026, 8), {100, 100}));
    EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
    EXPECT_TRUE(Storage.exists(monthFile("2026-07", 'a').c_str()));
    EXPECT_TRUE(Storage.exists(monthFile("2026-07", 'b').c_str()));
    EXPECT_FALSE(hasMonth("2026-08"));
  }
}
TEST_F(DailyReading, CleanupPreservesReadErrorsAndStopsOnDirectoryErrors) {
  ASSERT_TRUE(write(dateDay(2026, 7), {100, 100}));
  ASSERT_TRUE(write(dateDay(2026, 8), {100, 100}));
  faults.failScan = true;
  EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_TRUE(hasMonth("2026-07"));
  EXPECT_TRUE(hasMonth("2026-08"));
  faults.failScan = false;
  faults.readErrorPath = monthFile("2026-07", 'a');
  faults.readErrorAfter = 12;
  EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_TRUE(hasMonth("2026-07"));
  EXPECT_FALSE(hasMonth("2026-08"));
}
TEST_F(DailyReading, CleanupRequiresAcknowledgmentsToCoverBothCopies) {
  const uint32_t day = dateDay(2026, 8);
  ASSERT_TRUE(write(day, {100, 100}));
  ASSERT_TRUE(write(day, {200, 200}));
  // A valid but inconsistent older generation contains additional unacknowledged time.
  snapshotWord(Storage.files.at(monthFile("2026-08", 'a'))->bytes, 12, 300);
  EXPECT_FALSE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_TRUE(Storage.exists(monthFile("2026-08", 'a').c_str()));
  EXPECT_TRUE(Storage.exists(monthFile("2026-08", 'b').c_str()));
}
TEST_F(DailyReading, CleanupConvertsLegacyWithoutDeletingUnsyncedHistory) {
  seedLegacy(dateDay(2026, 7), {100, 100});
  seedLegacy(dateDay(2026, 8), {200, 100}, ".tmp");
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_FALSE(Storage.exists(legacyFile(dateDay(2026, 7)).c_str()));
  EXPECT_FALSE(hasMonth("2026-07"));
  EXPECT_TRUE(hasMonth("2026-08"));
  Counter c;
  ASSERT_TRUE(read(dateDay(2026, 8), c));
  EXPECT_EQ(c.seconds, 200u);
  EXPECT_EQ(c.uploaded, 100u);
  const size_t writes = faults.writes;
  ASSERT_TRUE(pruneSyncedMonths({2026, 10, 19}));
  EXPECT_EQ(faults.writes, writes);
}

TEST_F(DailyReading, PeriodicCheckpointsWaitForFiveMinutesAndExplicitFlushSavesTheRemainder) {
  const auto day = dayOf(17);
  ASSERT_TRUE(record(day, 299));
  Counter c;
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 0u);
  ASSERT_TRUE(record(day, 1));
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 300u);
  const auto writes = faults.writes;
  ASSERT_TRUE(record(day, 299));
  EXPECT_EQ(faults.writes, writes);
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 300u);
  ASSERT_TRUE(flush());  // Exit/restart/global save bypasses the periodic threshold.
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 599u);
  const auto flushedWrites = faults.writes;
  ASSERT_TRUE(flush());
  EXPECT_EQ(faults.writes, flushedWrites);
}

TEST_F(DailyReading, FailedFiveMinuteCheckpointRetainsAcceptedTimeForExplicitFlush) {
  const auto day = dayOf(17);
  ASSERT_TRUE(record(day, 299));
  faults.failOpenWrite = true;
  ASSERT_TRUE(record(day, 1));
  uint32_t seconds = 0;
  ASSERT_TRUE(secondsFor(day, seconds));
  EXPECT_EQ(seconds, 300u);
  EXPECT_FALSE(flush());
  faults.failOpenWrite = false;
  ASSERT_TRUE(flush());
  Counter c;
  ASSERT_TRUE(read(day, c));
  EXPECT_EQ(c.seconds, 300u);
}

TEST_F(DailyReading, DefersDueCheckpointWhilePagingWithoutLosingAcceptedSeconds) {
  Session s;
  s.start(dt(1, 12), 48);
  ASSERT_TRUE(s.accept(dt(1, 12, 5), 48, 300, 300, false));
  Counter disk;
  ASSERT_TRUE(read(dayOf(1), disk));
  EXPECT_EQ(disk.seconds, 0u);
  s.start(dt(1, 12, 5), 48);
  ASSERT_TRUE(s.accept(dt(1, 12, 5, 10), 48, 10, 310, false));
  ASSERT_TRUE(read(dayOf(1), disk));
  EXPECT_EQ(disk.seconds, 0u);
  faults.failOpenWrite = true;
  EXPECT_FALSE(checkpointIfDue());
  faults.failOpenWrite = false;
  ASSERT_TRUE(checkpointIfDue());
  ASSERT_TRUE(read(dayOf(1), disk));
  EXPECT_EQ(disk.seconds, 310u);
  ASSERT_TRUE(checkpointIfDue());
  EXPECT_EQ(secondsOn(1), 310u);
}
TEST_F(DailyReading, ExplicitFlushBypassesDeferralAcrossMidnight) {
  Session s;
  s.start(dt(1, 23, 59, 45), 48);
  ASSERT_TRUE(s.accept(dt(2, 0, 0, 15), 48, 30, 30, false));
  ASSERT_TRUE(checkpointIfDue());
  Counter disk;
  ASSERT_TRUE(read(dayOf(1), disk));
  EXPECT_EQ(disk.seconds, 0u);
  ASSERT_TRUE(flush());
  EXPECT_EQ(secondsOn(1), 15u);
  EXPECT_EQ(secondsOn(2), 15u);
}
