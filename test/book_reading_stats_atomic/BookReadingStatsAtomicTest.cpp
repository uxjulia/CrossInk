#include <BookReadingStats.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

namespace {
constexpr char CACHE_PATH[] = "/cache/book";
const std::string statsPath = std::string(CACHE_PATH) + "/stats_v5.bin";
const std::string previousStatsPath = std::string(CACHE_PATH) + "/stats_v4.bin";

BookReadingStats statsWithSeconds(const uint32_t seconds) {
  BookReadingStats stats;
  stats.totalReadingSeconds = seconds;
  return stats;
}
}  // namespace

class BookReadingStatsAtomicTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(BookReadingStatsAtomicTest, RestoresOriginalWhenReplacementRenameFails) {
  statsWithSeconds(123).save(CACHE_PATH);
  Storage.failNextRenameFrom(statsPath + ".tmp");

  statsWithSeconds(999).save(CACHE_PATH);

  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".bak").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, RecoversBackupAfterInterruptedReplacement) {
  statsWithSeconds(456).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));

  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".bak").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, PrefersCompleteTempFileOverOlderBackup) {
  statsWithSeconds(123).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));
  statsWithSeconds(456).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".tmp").c_str()));

  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, IgnoresPartialTempFileAndRecoversBackup) {
  statsWithSeconds(123).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));
  FsFile tempFile;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath + ".tmp", tempFile));
  ASSERT_EQ(tempFile.write(static_cast<uint8_t>(5)), 1U);
  ASSERT_TRUE(tempFile.close());

  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_TRUE(Storage.exists((statsPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".bak").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, RecoversTempWhenPublishAndBackupRestoreFail) {
  statsWithSeconds(123).save(CACHE_PATH);
  Storage.failNextRenameFrom(statsPath + ".tmp");
  Storage.failNextRenameFrom(statsPath + ".bak");

  statsWithSeconds(999).save(CACHE_PATH);

  EXPECT_FALSE(Storage.exists(statsPath.c_str()));
  EXPECT_TRUE(Storage.exists((statsPath + ".tmp").c_str()));
  EXPECT_TRUE(Storage.exists((statsPath + ".bak").c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 999U);
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, SuccessfulReplacementRemovesTransactionFiles) {
  statsWithSeconds(123).save(CACHE_PATH);
  statsWithSeconds(789).save(CACHE_PATH);

  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 789U);
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".bak").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, RemoveClearsRecoverableTransactionFiles) {
  statsWithSeconds(123).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));

  EXPECT_TRUE(BookReadingStats::remove(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((statsPath + ".bak").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, FailedBackupRemovalKeepsCanonicalStats) {
  statsWithSeconds(123).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));
  statsWithSeconds(456).save(CACHE_PATH);
  ASSERT_TRUE(Storage.exists(statsPath.c_str()));
  ASSERT_TRUE(Storage.exists((statsPath + ".bak").c_str()));
  Storage.failNextRemove(statsPath + ".bak");

  EXPECT_FALSE(BookReadingStats::remove(CACHE_PATH));

  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
}

TEST_F(BookReadingStatsAtomicTest, FailedFallbackRemovalKeepsCanonicalStats) {
  statsWithSeconds(123).save(CACHE_PATH);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), previousStatsPath.c_str()));
  statsWithSeconds(456).save(CACHE_PATH);
  ASSERT_TRUE(Storage.exists(statsPath.c_str()));
  ASSERT_TRUE(Storage.exists(previousStatsPath.c_str()));
  Storage.failNextRemove(previousStatsPath);

  EXPECT_FALSE(BookReadingStats::remove(CACHE_PATH));

  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
}
