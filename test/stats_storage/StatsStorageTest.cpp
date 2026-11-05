#include <HalStorage.h>
#include <esp_mac.h>
#include <gtest/gtest.h>

#include "BookReadingStats.h"
#include "DailyReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionJournal.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

void resetStatsStorageForTest();
namespace DailyReadingStats {
void resetForTest();
}

namespace {
constexpr const char* CACHE = "/.crosspoint/epub_book";
constexpr const char* LEGACY_BOOK = "/.crosspoint/epub_book/stats_v6.bin";
constexpr const char* ROOT_OWNER = "/.crosspoint/stats_origin.bin";

void switchReader(uint8_t device) {
  testMac[5] = device;
  failMac = false;
  resetStatsStorageForTest();
  DailyReadingStats::resetForTest();
}
void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  FsFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
  ASSERT_EQ(file.write(bytes.data(), bytes.size()), bytes.size());
  ASSERT_TRUE(file.close());
}
void legacyBook(uint32_t seconds) {
  std::vector<uint8_t> bytes(75);
  bytes[0] = 6;
  for (size_t i = 0; i < 4; ++i) bytes[3 + i] = seconds >> (i * 8);
  writeBytes(LEGACY_BOOK, bytes);
}
ReadingSessionJournal::Record session() {
  ReadingSessionJournal::Record r;
  strcpy(r.id, "0123456789abcdef0123456789abcdef");
  strcpy(r.document, "abcdef0123456789abcdef0123456789");
  r.startedAt = 1793491200;
  r.endedAt = r.startedAt + 60;
  r.seconds = 60;
  return r;
}
bool collectSession(void* context, const ReadingSessionJournal::Record& r) {
  static_cast<std::vector<ReadingSessionJournal::Record>*>(context)->push_back(r);
  return true;
}
bool collectDay(void* context, uint32_t, uint32_t seconds) {
  *static_cast<uint32_t*>(context) += seconds;
  return true;
}
}  // namespace
class StatsStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    switchReader(5);
  }
};

TEST_F(StatsStorageTest, CopiedBookStartsSeparateCountersAndSourceResumes) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  a.isCompleted = true;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(6);
  BookReadingStats b;
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE, b));
  EXPECT_EQ(b.totalReadingSeconds, 0u);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  b.totalReadingSeconds = 60;
  ASSERT_TRUE(b.save(CACHE));
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 60u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
}
TEST_F(StatsStorageTest, BookFolderCopiedWithoutRootMarkerRetainsItsOrigin) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  ASSERT_TRUE(a.save(CACHE));
  ASSERT_TRUE(Storage.remove(ROOT_OWNER));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  a.totalReadingSeconds = 60;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, LegacyStatsAreClaimedOnceAndNotReclaimedOnAnotherReader) {
  legacyBook(3600);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_FALSE(Storage.exists(LEGACY_BOOK));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, RootClaimProtectsPreviouslyUnopenedLegacyBooks) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  legacyBook(3600);
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, ReadOnlyCompletionDoesNotWriteOrMigrate) {
  legacyBook(3600);
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
}
TEST_F(StatsStorageTest, PendingSessionsAreNotUploadedByTheReceivingReader) {
  const auto record = session();
  ASSERT_TRUE(ReadingSessionJournal::save(record));
  switchReader(6);
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  EXPECT_TRUE(uploaded.empty());
  // Even the same session ID remains scoped to the original saved record.
  switchReader(5);
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_STREQ(uploaded[0].id, record.id);
  EXPECT_EQ(uploaded[0].seconds, 60u);
}
TEST_F(StatsStorageTest, BothReadersCanKeepIndependentPendingSessions) {
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  switchReader(6);
  auto b = session();
  b.seconds = 30;
  ASSERT_TRUE(ReadingSessionJournal::save(b));
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 30u);
  uploaded.clear();
  switchReader(5);
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 60u);
}
TEST_F(StatsStorageTest, DailyHistoryAndAcknowledgementsAreDeviceLocal) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 3600));
  ASSERT_TRUE(DailyReadingStats::flush());
  switchReader(6);
  uint32_t uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 0u);
  ASSERT_TRUE(DailyReadingStats::record(day, 60));
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 60u);
  switchReader(5);
  uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 3600u);
  uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 0u);
}
TEST_F(StatsStorageTest, GlobalMigrationRetainsBothSlotsAndInterruptedSaves) {
  for (const char* suffix : {"", ".bak", ".tmp"})
    writeBytes(std::string("/.crosspoint/global_stats.bin") + suffix, {1, 2, 3});
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  ASSERT_FALSE(path.empty());
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    EXPECT_TRUE(Storage.exists((path + suffix).c_str()));
    EXPECT_FALSE(Storage.exists((std::string("/.crosspoint/global_stats.bin") + suffix).c_str()));
  }
  switchReader(6);
  EXPECT_NE(StatsStorage::localPath("/.crosspoint/global_stats.bin"), path);
}
TEST_F(StatsStorageTest, InterruptedMigrationCannotOverwriteRecoveredHistory) {
  legacyBook(3600);
  faults.failRenameFrom = LEGACY_BOOK;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  BookReadingStats empty;
  EXPECT_FALSE(empty.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, FailedClaimDoesNotMoveOrOverwriteHistory) {
  legacyBook(3600);
  faults.failOpenWrite = true;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  faults.failOpenWrite = false;
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, CorruptOwnershipFailsClosed) {
  legacyBook(3600);
  writeBytes(ROOT_OWNER, {1, 2, 3});
  EXPECT_TRUE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}
TEST_F(StatsStorageTest, IdentityFailureNeverUsesGenericStats) {
  legacyBook(3600);
  failMac = true;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}
TEST_F(StatsStorageTest, ResetBookStatsPreservesOtherReadersHistory) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(6);
  a.totalReadingSeconds = 60;
  ASSERT_TRUE(a.save(CACHE));
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, LegacyDirectoryMovePreservesPendingSessions) {
  const auto owned = StatsStorage::localPath("/.crosspoint/reading_sessions", true);
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  ASSERT_TRUE(Storage.rename(owned.c_str(), "/.crosspoint/reading_sessions"));
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 60u);
  EXPECT_FALSE(Storage.exists("/.crosspoint/reading_sessions"));
}

TEST_F(StatsStorageTest, LifetimeTotalsUseOnlyTheCurrentReaderAndResetIsScoped) {
  GlobalReadingStats a;
  a.totalReadingSeconds = 3600;
  a.totalSessions = 10;
  ASSERT_TRUE(a.save());
  switchReader(6);
  GlobalReadingStats b;
  EXPECT_FALSE(GlobalReadingStats::loadForUpload(b));
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 0u);
  b.totalReadingSeconds = 60;
  b.totalSessions = 1;
  ASSERT_TRUE(b.save());
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(b));
  EXPECT_EQ(b.totalReadingSeconds, 60u);
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 0u);
  switchReader(5);
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(a));
  EXPECT_EQ(a.totalReadingSeconds, 3600u);
  EXPECT_EQ(a.totalSessions, 10u);
}

TEST_F(StatsStorageTest, ReadOnlyCompletionDoesNotPromoteAnInterruptedOriginClaim) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  ASSERT_TRUE(Storage.rename(ROOT_OWNER, "/.crosspoint/stats_origin.bin.tmp"));
  legacyBook(3600);
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
  EXPECT_TRUE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}

TEST_F(StatsStorageTest, TornInitialRootClaimRetriesWithoutLosingLegacyStats) {
  legacyBook(3600);
  writeBytes("/.crosspoint/stats_origin.bin.tmp", {1, 2, 3});
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_TRUE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_TRUE(Storage.exists(ROOT_OWNER));
  EXPECT_FALSE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
}
TEST_F(StatsStorageTest, TornInitialBookClaimRetriesUsingTheCardOrigin) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  legacyBook(3600);
  writeBytes(std::string(CACHE) + "/stats_origin.bin.tmp", {1, 2, 3});
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, UnreadableInitialClaimIsRetainedRatherThanTreatedAsTorn) {
  legacyBook(3600);
  const std::string temporary = "/.crosspoint/stats_origin.bin.tmp";
  writeBytes(temporary, {1, 2, 3});
  faults.failReadPath = temporary;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(temporary.c_str()));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  faults.failReadPath.clear();
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}

TEST_F(StatsStorageTest, EmptyInitialClaimCanRetryAfterPowerLossBeforeTheFirstByte) {
  legacyBook(3600);
  writeBytes("/.crosspoint/stats_origin.bin.tmp", {});
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, UnsupportedCompleteTemporaryClaimIsNeverReplaced) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  auto bytes = Storage.files.at(ROOT_OWNER)->bytes;
  bytes[0] = 2;
  const uint32_t crc = SyncFiles::crc(bytes.data(), 33, 0);
  for (size_t i = 0; i < 4; ++i) bytes[33 + i] = crc >> (8 * i);
  ASSERT_TRUE(Storage.remove(ROOT_OWNER));
  writeBytes("/.crosspoint/stats_origin.bin.tmp", bytes);
  legacyBook(3600);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  EXPECT_EQ(Storage.files.at("/.crosspoint/stats_origin.bin.tmp")->bytes, bytes);
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
}
