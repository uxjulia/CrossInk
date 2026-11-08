#include <HalStorage.h>
#include <esp_mac.h>
#include <gtest/gtest.h>

#include "BookReadingStats.h"
#include "DailyReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionJournal.h"
#include "TwoSlotFile.h"
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

TEST_F(StatsStorageTest, LegacyBookRecreatedByOlderFirmwareKeepsLocalHistoryWritable) {
  BookReadingStats saved;
  saved.totalReadingSeconds = 565;
  saved.startDate = {2026, 11, 5};
  saved.avgSecondsPerForwardPage = 30;
  saved.paceSampleCount = 18;
  ASSERT_TRUE(saved.save(CACHE));
  legacyBook(95);
  const auto legacy = Storage.files.at(LEGACY_BOOK)->bytes;
  switchReader(5);
  auto loaded = BookReadingStats::load(CACHE);
  ASSERT_EQ(loaded.totalReadingSeconds, 565u);
  EXPECT_EQ(loaded.startDate.year, 2026);
  EXPECT_EQ(loaded.paceSampleCount, 18u);
  loaded.totalReadingSeconds += 120;
  ASSERT_TRUE(loaded.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 685u);
  EXPECT_EQ(Storage.files.at(LEGACY_BOOK)->bytes, legacy);
}

TEST_F(StatsStorageTest, LegacyGlobalCollisionDoesNotDisableBookStats) {
  GlobalReadingStats global;
  global.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  writeBytes("/.crosspoint/global_stats.bin", {1, 2, 3});
  switchReader(5);
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(global));
  EXPECT_EQ(global.totalReadingSeconds, 3600u);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 565u);
  global.totalReadingSeconds += 60;
  EXPECT_TRUE(global.save());
}

TEST_F(StatsStorageTest, ExistingLocalRecoverySlotKeepsLegacySlotsSeparate) {
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    Storage.reset();
    switchReader(5);
    const auto local = StatsStorage::localPath(LEGACY_BOOK);
    writeBytes(local + suffix, {6, 7, 8});
    legacyBook(95);
    writeBytes(std::string(LEGACY_BOOK) + ".bak", {1, 2, 3});
    writeBytes(std::string(LEGACY_BOOK) + ".tmp", {4, 5, 6});
    EXPECT_EQ(StatsStorage::localPath(LEGACY_BOOK), local);
    EXPECT_EQ(StatsStorage::localPath(LEGACY_BOOK, false, true), local);
    EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
    EXPECT_TRUE(Storage.exists((std::string(LEGACY_BOOK) + ".bak").c_str()));
    EXPECT_TRUE(Storage.exists((std::string(LEGACY_BOOK) + ".tmp").c_str()));
  }
}

TEST_F(StatsStorageTest, RecreatedLegacyDailyAndSessionDirectoriesDoNotBlockLocalWrites) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 3600));
  ASSERT_TRUE(DailyReadingStats::flush());
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/daily_reading"));
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/reading_sessions"));
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::record(day, 60));
  ASSERT_TRUE(DailyReadingStats::flush());
  uint32_t uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 3660u);
  auto record = session();
  record.seconds = 120;
  record.endedAt = record.startedAt + record.seconds;
  ASSERT_TRUE(ReadingSessionJournal::save(record));
  std::vector<ReadingSessionJournal::Record> sessions;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions[0].seconds, 120u);
  EXPECT_TRUE(Storage.exists("/.crosspoint/daily_reading"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/reading_sessions"));
}

TEST_F(StatsStorageTest, InterruptedMigrationStillRecoversTheNewerLegacyBackup) {
  BookReadingStats book;
  book.totalReadingSeconds = 100;
  ASSERT_TRUE(book.save(CACHE));
  book.totalReadingSeconds = 200;
  ASSERT_TRUE(book.save(CACHE));
  const std::string generic = std::string(CACHE) + "/stats_v7.bin";
  const auto local = StatsStorage::localPath(generic);
  ASSERT_TRUE(Storage.rename(local.c_str(), generic.c_str()));
  ASSERT_TRUE(Storage.rename((local + ".bak").c_str(), (generic + ".bak").c_str()));
  faults.failRenameFrom = generic + ".bak";
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(local.c_str()));
  EXPECT_TRUE(Storage.exists((generic + ".bak").c_str()));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 200u);
  EXPECT_FALSE(Storage.exists((generic + ".bak").c_str()));
}

TEST_F(StatsStorageTest, ResetCannotRevivePreservedLegacyStatsOrEraseAnotherReader) {
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  const auto generic = std::string(CACHE) + "/stats_v7.bin";
  auto recreated = std::vector<uint8_t>(91);
  recreated[0] = 7;
  recreated[3] = 95;
  writeBytes(generic, recreated);
  switchReader(6);
  book.totalReadingSeconds = 60;
  ASSERT_TRUE(book.save(CACHE));
  switchReader(5);
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(generic.c_str()));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 60u);
}

TEST_F(StatsStorageTest, FailedResetWriteKeepsNewestHistoryWithLegacyCollision) {
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  legacyBook(95);
  faults.bytesUntilFailure = 0;
  EXPECT_FALSE(BookReadingStats::remove(CACHE));
  faults.bytesUntilFailure = -1;
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 565u);
}

TEST_F(StatsStorageTest, FreshCountersKeepTheirIdsAcrossCheckpointsAndRestoredCards) {
  auto global = GlobalReadingStats::load();
  auto book = BookReadingStats::load(CACHE);
  EXPECT_NE(global.counterId.bytes, StatsCounterId{}.bytes);
  EXPECT_NE(book.counterId.bytes, global.counterId.bytes);
  global.totalReadingSeconds = book.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  ASSERT_TRUE(book.save(CACHE));
  std::map<std::string, HostFileData> backup;
  for (const auto& [path, data] : Storage.files) backup.emplace(path, *data);
  const auto globalId = global.counterId.bytes;
  const auto bookId = book.counterId.bytes;
  switchReader(5);  // reboot
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_EQ(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  EXPECT_NE(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_NE(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  Storage.reset();
  for (const auto& [path, data] : backup) Storage.files.emplace(path, std::make_shared<HostFileData>(data));
  switchReader(5);
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_EQ(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 3600u);
}

TEST_F(StatsStorageTest, ResetFailureRetainsThePreviousCounterIdentity) {
  auto global = GlobalReadingStats::load();
  global.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  faults.bytesUntilFailure = 0;
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  faults.bytesUntilFailure = -1;
  const auto recovered = GlobalReadingStats::load();
  EXPECT_EQ(recovered.totalReadingSeconds, 3600u);
  EXPECT_EQ(recovered.counterId.bytes, global.counterId.bytes);
}

TEST_F(StatsStorageTest, UpgradingVersionThreeSlotsRetainsNewestCountersAndLegacyIdentity) {
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  for (uint32_t seq = 1; seq <= 2; ++seq) {
    std::vector<uint8_t> bytes(167);
    bytes[0] = 3;
    progress_record::writeU32(bytes.data() + 5, seq * 3600);
    progress_record::writeU32(bytes.data() + 159, seq);
    progress_record::writeU32(bytes.data() + 163, progress_record::crc32(bytes.data(), 163));
    writeBytes(path + (seq == 1 ? "" : ".bak"), bytes);
  }
  auto stats = GlobalReadingStats::load();
  ASSERT_EQ(stats.totalReadingSeconds, 7200u);
  EXPECT_EQ(stats.counterId.bytes, StatsCounterId{}.bytes);
  stats.totalReadingSeconds += 60;
  ASSERT_TRUE(stats.save());
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 7260u);
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, StatsCounterId{}.bytes);
  // A failed new-format save must still recover the previous old-format slot.
  faults.bytesUntilFailure = 20;
  stats.totalReadingSeconds += 60;
  EXPECT_FALSE(stats.save());
  faults.bytesUntilFailure = -1;
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 7260u);
}

TEST_F(StatsStorageTest, DailyIdentitySurvivesGlobalResetAndChangesForAFreshCard) {
  StatsCounterId before, after;
  ASSERT_TRUE(DailyReadingStats::counterId(before));
  EXPECT_NE(before.bytes, StatsCounterId{}.bytes);
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 600));
  ASSERT_TRUE(DailyReadingStats::flush());
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_EQ(after.bytes, before.bytes);
  Storage.reset();  // fresh SD on the same hardware
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_NE(after.bytes, before.bytes);
}

TEST_F(StatsStorageTest, ExistingDailyHistoryAdoptsLegacyIdentityAndNeverChangesIt) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 600));
  ASSERT_TRUE(DailyReadingStats::flush());
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  ASSERT_TRUE(Storage.remove(path.c_str()));  // simulate a pre-ID daily directory
  switchReader(5);
  StatsCounterId id;
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_EQ(id.bytes, StatsCounterId{}.bytes);
  uint32_t seconds = 0;
  ASSERT_TRUE(DailyReadingStats::secondsFor(day, seconds));
  EXPECT_EQ(seconds, 600u);
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_EQ(id.bytes, StatsCounterId{}.bytes);
}

TEST_F(StatsStorageTest, UncertainDailyIdentityNeverStartsAnotherLifetime) {
  StatsCounterId id;
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  const auto bytes = Storage.files.at(path)->bytes;
  for (const auto& bad : {std::vector<uint8_t>{}, std::vector<uint8_t>{2}, std::vector<uint8_t>{1, 2, 3}}) {
    writeBytes(path, bad);
    switchReader(5);
    EXPECT_FALSE(DailyReadingStats::counterId(id));
  }
  writeBytes(path, bytes);
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  faults.failReadPath = path;
  switchReader(5);
  EXPECT_FALSE(DailyReadingStats::counterId(id));
}

TEST_F(StatsStorageTest, DailyIdentityPublicationFailureKeepsReadingPending) {
  ASSERT_FALSE(StatsStorage::localPath(DailyReadingStats::DIRECTORY, true).empty());
  faults.failOpenWrite = true;
  EXPECT_FALSE(DailyReadingStats::record(readingStatsDayIndex({2026, 11, 1}), 60));
  faults.failOpenWrite = false;
  StatsCounterId id;
  EXPECT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_TRUE(DailyReadingStats::record(readingStatsDayIndex({2026, 11, 1}), 60));
}

TEST_F(StatsStorageTest, DailyIdentityRecoversACompleteUnpublishedMarker) {
  StatsCounterId before, after;
  ASSERT_TRUE(DailyReadingStats::counterId(before));
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  ASSERT_TRUE(Storage.rename(path.c_str(), (path + ".tmp").c_str()));
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_EQ(after.bytes, before.bytes);
  EXPECT_TRUE(Storage.exists(path.c_str()));
}

TEST_F(StatsStorageTest, FailedDailyDirectoryScanNeverInventsAnIdentity) {
  const auto directory = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true);
  ASSERT_TRUE(Storage.mkdir(directory.c_str()));
  faults.failScan = true;
  StatsCounterId id;
  EXPECT_FALSE(DailyReadingStats::counterId(id));
  EXPECT_FALSE(Storage.exists((directory + "/counter_id.bin").c_str()));
  faults.failScan = false;
  EXPECT_TRUE(DailyReadingStats::counterId(id));
}
