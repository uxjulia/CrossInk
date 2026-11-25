#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <set>

#include "ReadingSessionJournal.h"
using namespace ReadingSessionJournal;
class Journal : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};
static ReadingStatsDateTime dt(uint8_t hour = 12, uint8_t minute = 0, uint8_t second = 0, uint8_t day = 1) {
  return {{2026, 10, day}, hour, minute, second};
}
static bool collect(void* ptr, const Record& r) {
  static_cast<std::vector<Record>*>(ptr)->push_back(r);
  return true;
}
static Record savedRecord(unsigned index) {
  Record record;
  snprintf(record.id, sizeof(record.id), "%032x", index);
  strcpy(record.document, "0123456789abcdef0123456789abcdef");
  record.startedAt = 946684800ULL + index * 120;
  record.endedAt = record.startedAt + 60;
  record.seconds = 60;
  return record;
}
TEST_F(Journal, DrainsTheEntireQueueInOneUpload) {
  for (const unsigned count : {0u, 1u, 20u, 21u, 40u, 75u}) {
    SCOPED_TRACE(count);
    Storage.reset();
    for (unsigned i = 0; i < count; ++i) ASSERT_TRUE(save(savedRecord(i)));
    std::vector<Record> rows;
    ASSERT_TRUE(uploadPending(collect, &rows));
    ASSERT_EQ(rows.size(), count);
    std::set<std::string> ids;
    for (const auto& row : rows) EXPECT_TRUE(ids.insert(row.id).second);
    for (unsigned i = 0; i < count; ++i) EXPECT_EQ(ids.count(savedRecord(i).id), 1u);
    rows.clear();
    EXPECT_TRUE(uploadPending(collect, &rows));
    EXPECT_TRUE(rows.empty());
  }
}
TEST_F(Journal, ResumesAfterAFailedUploadBeyondTheFormerLimit) {
  for (unsigned i = 0; i < 45; ++i) ASSERT_TRUE(save(savedRecord(i)));
  std::vector<Record> attempts;
  ASSERT_FALSE(uploadPending(
      [](void* ptr, const Record& record) {
        auto& rows = *static_cast<std::vector<Record>*>(ptr);
        rows.push_back(record);
        return rows.size() <= 25;
      },
      &attempts));
  ASSERT_EQ(attempts.size(), 26u);
  std::vector<Record> retry;
  ASSERT_TRUE(uploadPending(collect, &retry));
  ASSERT_EQ(retry.size(), 20u);
  std::set<std::string> ids;
  for (size_t i = 0; i < 25; ++i) EXPECT_TRUE(ids.insert(attempts[i].id).second);
  for (const auto& row : retry) EXPECT_TRUE(ids.insert(row.id).second);
  EXPECT_EQ(ids.size(), 45u);
  bool retriedFailure = false;
  for (const auto& row : retry) {
    if (strcmp(row.id, attempts.back().id) == 0) retriedFailure = true;
  }
  EXPECT_TRUE(retriedFailure);
  retry.clear();
  EXPECT_TRUE(uploadPending(collect, &retry));
  EXPECT_TRUE(retry.empty());
}
TEST_F(Journal, PersistsACheckpointAndRemovesOnlyAfterAcknowledgment) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5), 48, 300));
  std::vector<Record> first;
  ASSERT_FALSE(uploadPending(
      [](void* p, const Record& r) {
        collect(p, r);
        return false;
      },
      &first));
  ASSERT_EQ(first.size(), 1u);
  std::vector<Record> retry;
  ASSERT_TRUE(uploadPending(collect, &retry));
  ASSERT_EQ(retry.size(), 1u);
  EXPECT_STREQ(first[0].id, retry[0].id);
  EXPECT_EQ(retry[0].seconds, 300u);
  EXPECT_EQ(retry[0].endedAt - retry[0].startedAt, 300u);
  std::vector<Record> empty;
  EXPECT_TRUE(uploadPending(collect, &empty));
  EXPECT_TRUE(empty.empty());
}
TEST_F(Journal, SeparatesPausedIntervalsWithoutInventingReadingAcrossTheGap) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 0, 20), 48, 20));
  s.start(dt(12, 5), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5, 20), 48, 20));
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].seconds, 20u);
  EXPECT_EQ(rows[1].seconds, 20u);
  EXPECT_EQ(rows[0].endedAt - rows[0].startedAt, 20u);
  EXPECT_EQ(rows[1].endedAt - rows[1].startedAt, 20u);
}
TEST_F(Journal, RetainsFailedWritesWithTheSameId) {
  Session s;
  s.start(dt(), 48);
  faults.failOpenWrite = true;
  EXPECT_FALSE(s.accept("/book.epub", dt(12, 5), 48, 300));
  faults.failOpenWrite = false;
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seconds, 300u);
}
TEST_F(Journal, KeepsUncertainDataAfterCorruptionAndFailedDelete) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5), 48, 300));
  std::vector<Record> rows;
  ASSERT_FALSE(uploadPending(
      [](void* p, const Record& r) {
        collect(p, r);
        return false;
      },
      &rows));
  const std::string path = "/.crosspoint/reading_sessions/" + std::string(rows[0].id) + ".bin";
  Storage.failNextRemove(path);
  EXPECT_FALSE(uploadPending(collect, &rows));
  EXPECT_TRUE(Storage.exists(path.c_str()));
  Storage.files[path]->bytes[2] ^= 1;
  EXPECT_FALSE(uploadPending(collect, &rows));
  EXPECT_TRUE(Storage.exists(path.c_str()));
}
TEST_F(Journal, HandlesMidnightAndOffsetWithoutInventingTimeWhenClockChanges) {
  Session s;
  s.start(dt(23, 59, 40), 64);
  ASSERT_TRUE(s.accept("/book.epub", dt(0, 0, 10, 2), 64, 30));
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].utcOffsetMinutes, 240);
  EXPECT_EQ(rows[0].seconds, 30u);
  s.start(dt(), 48);
  EXPECT_TRUE(s.accept("/book.epub", dt(13), 48, 60));
  s.start(dt(), 48);
  EXPECT_TRUE(s.accept("/book.epub", dt(12, 1), 52, 60));
  ASSERT_TRUE(s.flush());
  rows.clear();
  ASSERT_TRUE(uploadPending(collect, &rows));
  EXPECT_TRUE(rows.empty());
}
TEST_F(Journal, FlushesShortAcceptedSessionsButKeepsBelowThresholdUnexported) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 0, 15), 48, 15));
  ASSERT_TRUE(s.flush());
  s.start(dt(13), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(13, 0, 5), 48, 5));
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seconds, 15u);
}

TEST_F(Journal, LaterCheckpointsRetainTheSessionIdAndCumulativeDuration) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5), 48, 300));
  std::vector<Record> first;
  ASSERT_TRUE(uploadPending(collect, &first));
  ASSERT_EQ(first.size(), 1u);
  s.start(dt(12, 5), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 10), 48, 300));
  std::vector<Record> second;
  ASSERT_TRUE(uploadPending(collect, &second));
  ASSERT_EQ(second.size(), 1u);
  EXPECT_STREQ(first[0].id, second[0].id);
  EXPECT_EQ(second[0].seconds, 600u);
  ASSERT_TRUE(s.flush());
  std::vector<Record> empty;
  ASSERT_TRUE(uploadPending(collect, &empty));
  EXPECT_TRUE(empty.empty());
}

TEST_F(Journal, ACorrectedFutureClockDoesNotBlockOtherSessions) {
  Record future;
  strcpy(future.id, "00000000000000000000000000000001");
  strcpy(future.document, "0123456789abcdef0123456789abcdef");
  future.startedAt = 4070952000ULL;
  future.endedAt = future.startedAt + 60;
  future.seconds = 60;
  ASSERT_TRUE(save(future));
  Session current;
  current.start(dt(), 48);
  ASSERT_TRUE(current.accept("/book.epub", dt(12, 1), 48, 60));
  ASSERT_TRUE(current.checkpoint());
  std::vector<Record> rows;
  EXPECT_FALSE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_STRNE(rows[0].id, future.id);
  EXPECT_TRUE(Storage.exists("/.crosspoint/reading_sessions/00000000000000000000000000000001.bin"));
}
TEST_F(Journal, PausedReadingAcrossMidnightKeepsEachDaysMeasuredTime) {
  Session s;
  s.start(dt(23, 50), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(23, 51), 48, 60));
  s.start(dt(0, 10, 0, 2), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(0, 20, 0, 2), 48, 600));
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].seconds + rows[1].seconds, 660u);
  for (const auto& row : rows) EXPECT_EQ(row.endedAt - row.startedAt, row.seconds);
}

TEST_F(Journal, CheckpointRevisionAdvancesAcrossNewIdentitiesWithShorterDurations) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 10), 48, 600));
  const auto first = s.revision();
  ASSERT_GT(first, 0u);
  s.start(dt(13), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(13, 5), 48, 300));
  EXPECT_GT(s.revision(), first);
}

TEST_F(Journal, WaitsForFiveMinutesOfAcceptedReadingBetweenCheckpoints) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 4, 59), 48, 299));
  EXPECT_EQ(s.revision(), 0u);
  EXPECT_EQ(faults.writes, 0u);
  s.start(dt(12, 4, 59), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5), 48, 1));
  EXPECT_EQ(s.revision(), 1u);
  const auto writes = faults.writes;
  EXPECT_GT(writes, 0u);
  s.start(dt(12, 5), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 9, 59), 48, 299));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_EQ(s.revision(), 1u);
  s.start(dt(12, 9, 59), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 10), 48, 1));
  EXPECT_GT(faults.writes, writes);
  EXPECT_EQ(s.revision(), 2u);
}

TEST_F(Journal, ExplicitCheckpointAndExitSaveBeforeThePeriodicThreshold) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 0, 30), 48, 30));
  ASSERT_EQ(faults.writes, 0u);
  ASSERT_TRUE(s.checkpoint());  // Restart handoff preserves the live session.
  EXPECT_EQ(s.revision(), 1u);
  const auto writes = faults.writes;
  ASSERT_TRUE(s.checkpoint());
  EXPECT_EQ(faults.writes, writes);
  s.start(dt(12, 0, 30), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 1), 48, 30));
  EXPECT_EQ(faults.writes, writes);
  ASSERT_TRUE(s.flush());  // Normal exit persists the remaining half-minute.
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seconds, 60u);
}

TEST_F(Journal, DefersDueCheckpointWhilePagingAndRetriesWithoutDuplicatingTime) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5), 48, 300, false));
  EXPECT_EQ(s.revision(), 0u);
  EXPECT_TRUE(Storage.files.empty());
  s.start(dt(12, 5), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 5, 10), 48, 10, false));
  EXPECT_TRUE(Storage.files.empty());
  faults.failOpenWrite = true;
  EXPECT_FALSE(s.checkpointIfDue());
  EXPECT_EQ(s.revision(), 0u);
  faults.failOpenWrite = false;
  ASSERT_TRUE(s.checkpointIfDue());
  EXPECT_EQ(s.revision(), 1u);
  ASSERT_TRUE(s.checkpointIfDue());
  EXPECT_EQ(s.revision(), 1u);
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seconds, 310u);
}
TEST_F(Journal, ExplicitFlushBypassesDeferralBeforeFiveMinutes) {
  Session s;
  s.start(dt(), 48);
  ASSERT_TRUE(s.accept("/book.epub", dt(12, 0, 20), 48, 20, false));
  ASSERT_TRUE(s.checkpointIfDue());
  EXPECT_TRUE(Storage.files.empty());
  ASSERT_TRUE(s.flush());
  std::vector<Record> rows;
  ASSERT_TRUE(uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seconds, 20u);
}
