#include <BookReadingStats.h>
#include <HalStorage.h>
#include <ReadingPace.h>
#include <gtest/gtest.h>

#include <string>

namespace {
constexpr char CACHE_PATH[] = "/cache/book";
const std::string statsPath = std::string(CACHE_PATH) + "/stats_v6.bin";
const std::string previousStatsPath = std::string(CACHE_PATH) + "/stats_v5.bin";

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

TEST_F(BookReadingStatsAtomicTest, SavesAlternateWithoutRename) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  const auto first = Storage.data(statsPath).bytes;
  Storage.failNextRenameFrom(statsPath + ".tmp");
  ASSERT_TRUE(statsWithSeconds(456).save(CACHE_PATH));
  EXPECT_EQ(Storage.data(statsPath).bytes, first);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
  ASSERT_TRUE(statsWithSeconds(789).save(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 789U);
}

TEST_F(BookReadingStatsAtomicTest, TornSavePreservesNewestCompleteSlot) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  ASSERT_TRUE(statsWithSeconds(456).save(CACHE_PATH));
  Storage.data(statsPath).writeLimit = 20;
  EXPECT_FALSE(statsWithSeconds(999).save(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
}

TEST_F(BookReadingStatsAtomicTest, FirstUpgradePreservesSoleLegacyCopy) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  Storage.data(statsPath).bytes.resize(75);  // legacy v6, no slot trailer
  auto backup = Storage.open((statsPath + ".bak").c_str(), O_RDWR | O_CREAT);
  backup.close();
  Storage.data(statsPath + ".bak").writeLimit = 20;
  EXPECT_FALSE(statsWithSeconds(456).save(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
  Storage.data(statsPath + ".bak").writeLimit = SIZE_MAX;
  ASSERT_TRUE(statsWithSeconds(456).save(CACHE_PATH));
  EXPECT_EQ(Storage.data(statsPath).bytes.size(), 75u);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
}

TEST_F(BookReadingStatsAtomicTest, CrcFailureFallsBackAndCompletionPeekUsesNewest) {
  auto stats = statsWithSeconds(123);
  ASSERT_TRUE(stats.save(CACHE_PATH));
  stats.isCompleted = true;
  ASSERT_TRUE(stats.save(CACHE_PATH));
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE_PATH));
  Storage.data(statsPath + ".bak").bytes[3] ^= 1;
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE_PATH));
}

TEST_F(BookReadingStatsAtomicTest, LegacyCompleteTempStillRecoversBeforeBackup) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  Storage.data(statsPath).bytes.resize(75);
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));
  auto temp = Storage.open((statsPath + ".tmp").c_str(), O_RDWR | O_CREAT);
  auto bytes = Storage.data(statsPath + ".bak").bytes;
  bytes[3] = 42;
  ASSERT_EQ(temp.write(bytes.data(), bytes.size()), bytes.size());
  temp.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 42U);
}

TEST_F(BookReadingStatsAtomicTest, FailedSyncDoesNotTouchPreviousSlot) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  const auto previous = Storage.data(statsPath).bytes;
  auto backup = Storage.open((statsPath + ".bak").c_str(), O_RDWR | O_CREAT);
  backup.close();
  Storage.data(statsPath + ".bak").failSync = true;
  EXPECT_FALSE(statsWithSeconds(456).save(CACHE_PATH));
  EXPECT_EQ(Storage.data(statsPath).bytes, previous);
}

TEST_F(BookReadingStatsAtomicTest, IdenticalSaveSkipsWrite) {
  statsWithSeconds(123).save(CACHE_PATH);
  // An unchanged payload must leave even an invalid inactive slot untouched.
  FsFile marker;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath + ".bak", marker));
  ASSERT_EQ(marker.write(static_cast<uint8_t>(7)), 1U);
  ASSERT_TRUE(marker.close());

  statsWithSeconds(123).save(CACHE_PATH);
  EXPECT_EQ(Storage.data(statsPath + ".bak").bytes, std::vector<uint8_t>{7});

  statsWithSeconds(124).save(CACHE_PATH);
  EXPECT_EQ(Storage.data(statsPath + ".bak").bytes.size(), 83u);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 124U);
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

TEST_F(BookReadingStatsAtomicTest, UploadRejectsMissingTruncatedAndNewerSnapshots) {
  BookReadingStats snapshot;
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, snapshot));
  FsFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath, file));
  ASSERT_EQ(file.write(static_cast<uint8_t>(5)), 1U);
  file.close();
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, snapshot));
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  ASSERT_TRUE(BookReadingStats::loadForUpload(CACHE_PATH, snapshot));
  EXPECT_EQ(snapshot.totalReadingSeconds, 123U);
  // Oversized future payload must not be accepted merely because its first
  // 75 bytes look like the current format.
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath, file));
  uint8_t oversized[76]{};
  oversized[0] = 6;
  file.write(oversized, sizeof(oversized));
  file.close();
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, snapshot));
}

TEST_F(BookReadingStatsAtomicTest, UploadAcceptsAnExplicitlySavedEmptySnapshot) {
  ASSERT_TRUE(BookReadingStats{}.save(CACHE_PATH));
  BookReadingStats snapshot;
  EXPECT_TRUE(BookReadingStats::loadForUpload(CACHE_PATH, snapshot));
  EXPECT_EQ(snapshot.totalReadingSeconds, 0U);
}

TEST_F(BookReadingStatsAtomicTest, FractionSurvivesSaveAndReopen) {
  BookReadingStats stats;
  stats.recordForwardPageRead(29);
  stats.recordForwardPageRead(30);
  EXPECT_EQ(stats.averageForwardPageMillis(), 29500U);
  ASSERT_TRUE(stats.save(CACHE_PATH));
  const auto loaded = BookReadingStats::load(CACHE_PATH);
  EXPECT_EQ(loaded.averageForwardPageMillis(), 29500U);
  EXPECT_EQ(loaded.paceSampleCount, 2U);
}

TEST_F(BookReadingStatsAtomicTest, AlternatingSamplesDoNotRoundDownEachUpdate) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(30000);
  stats.paceSampleCount = 100;
  for (int i = 0; i < 20; ++i) stats.recordForwardPageRead(i % 2 == 0 ? 29 : 31);
  EXPECT_NEAR(stats.averageForwardPageMillis(), 30000U, 5);
}

TEST_F(BookReadingStatsAtomicTest, EstablishedPaceCanMoveInBothDirections) {
  for (const uint32_t sample : {20U, 40U}) {
    BookReadingStats stats;
    stats.setAverageForwardPageMillis(30000);
    stats.paceSampleCount = 100;
    for (int i = 0; i < 20; ++i) stats.recordForwardPageRead(sample);
    const double expectedMillis = (100.0 * 30000 + 20.0 * sample * 1000) / 120;
    EXPECT_NEAR(stats.averageForwardPageMillis(), expectedMillis, 10);
  }
}

TEST_F(BookReadingStatsAtomicTest, SaturatedCountAndMaximumSampleDoNotOverflow) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(60000000);
  stats.paceSampleCount = 1000;
  stats.recordForwardPageRead(UINT32_MAX);
  EXPECT_EQ(stats.paceSampleCount, 1000U);
  EXPECT_GT(stats.averageForwardPageMillis(), 60000000U);
  EXPECT_LE(stats.averageForwardPageMillis(), 65535000U);
  stats.setAverageForwardPageMillis(UINT32_MAX);
  EXPECT_EQ(stats.averageForwardPageMillis(), 65535000U);
  EXPECT_EQ(stats.paceFractionMillis, 0U);
  stats.recordForwardPageRead(0);
  EXPECT_EQ(stats.averageForwardPageMillis(), 65535000U);
}

namespace {
void writeOldStats(const std::string& path, const uint8_t version, const size_t size) {
  uint8_t data[73]{};
  data[0] = version;
  data[1] = 7;    // sessions
  data[3] = 123;  // reading seconds
  if (size >= 12) data[11] = 1;
  if (size >= 16) {
    data[12] = 30;
    data[14] = 100;
  }
  if (size >= 69) {
    data[16] = 1;
    data[17] = 0xea;  // 2026
    data[18] = 7;
    data[19] = 10;
    data[20] = 22;
    data[25] = 55;
    data[41] = 66;
  }
  if (size >= 73) data[69] = 42;
  FsFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
  ASSERT_EQ(file.write(data, size), size);
  ASSERT_TRUE(file.close());
}
}  // namespace

TEST_F(BookReadingStatsAtomicTest, MigratesEveryOlderPayloadWithoutLosingHistory) {
  const size_t sizes[] = {11, 12, 16, 69, 73};
  for (uint8_t version = 1; version <= 5; ++version) {
    Storage.reset();
    const std::string path = version >= 4 ? std::string(CACHE_PATH) + "/stats_v" + std::to_string(version) + ".bin"
                                          : std::string(CACHE_PATH) + "/stats.bin";
    writeOldStats(path, version, sizes[version - 1]);
    auto stats = BookReadingStats::load(CACHE_PATH);
    EXPECT_EQ(stats.sessionCount, 7U);
    EXPECT_EQ(stats.totalReadingSeconds, 123U);
    EXPECT_EQ(stats.isCompleted, version >= 2);
    if (version >= 3) EXPECT_EQ(stats.averageForwardPageMillis(), 30000U);
    if (version >= 4) {
      EXPECT_TRUE(stats.startDateManual);
      EXPECT_EQ(stats.startDate.year, 2026U);
      EXPECT_EQ(stats.timeOfDaySeconds[0], 55U);
      EXPECT_EQ(stats.dayOfWeekSeconds[0], 66U);
    }
    if (version >= 5) EXPECT_EQ(stats.estimatedTimeLeftSeconds, 42U);
    ASSERT_TRUE(stats.save(CACHE_PATH));
    EXPECT_TRUE(Storage.exists(path.c_str()));
    EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
    EXPECT_EQ(BookReadingStats::peekCompleted(CACHE_PATH), version >= 2);
  }
}

TEST_F(BookReadingStatsAtomicTest, RecoversVersionFiveTransactionsDuringUpgrade) {
  for (const char* suffix : {".tmp", ".bak"}) {
    Storage.reset();
    writeOldStats(previousStatsPath + suffix, 5, 73);
    EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE_PATH));
    EXPECT_FALSE(Storage.exists(previousStatsPath.c_str()));  // peek is read-only
    const auto stats = BookReadingStats::load(CACHE_PATH);
    EXPECT_EQ(stats.totalReadingSeconds, 123U);
    EXPECT_EQ(stats.averageForwardPageMillis(), 30000U);
    EXPECT_TRUE(Storage.exists(previousStatsPath.c_str()));
  }
}

TEST_F(BookReadingStatsAtomicTest, IncompleteOldTempDoesNotHideOldBackup) {
  writeOldStats(previousStatsPath + ".tmp", 5, 16);
  writeOldStats(previousStatsPath + ".bak", 5, 73);
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
}

TEST_F(BookReadingStatsAtomicTest, ResetRemovesAllSupportedFallbacksAndTransactions) {
  ASSERT_TRUE(statsWithSeconds(456).save(CACHE_PATH));
  for (const uint8_t version : {4, 5}) {
    for (const char* suffix : {"", ".tmp", ".bak"}) {
      writeOldStats(std::string(CACHE_PATH) + "/stats_v" + std::to_string(version) + ".bin" + suffix, version,
                    version == 4 ? 69 : 73);
    }
  }
  EXPECT_TRUE(BookReadingStats::remove(CACHE_PATH));
  BookReadingStats stats;
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, stats));
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE_PATH));
}

TEST_F(BookReadingStatsAtomicTest, RejectsInvalidFraction) {
  BookReadingStats stats;
  stats.paceFractionMillis = 1000;
  ASSERT_TRUE(stats.save(CACHE_PATH));
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, stats));
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE_PATH));
}

TEST(ReadingPace, NewReaderKeepsThreeSampleWarmup) {
  EXPECT_EQ(ReadingPace::estimateMillis(0, 0, 60, 2), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(0, 0, 90, 3), 30000U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 2, 0, 0), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 3, 0, 0), 30000U);
}

TEST(ReadingPace, FasterAndSlowerSessionsHaveNoTenSampleCliff) {
  for (const uint32_t sample : {20U, 40U}) {
    uint32_t previous = 30000;
    for (uint16_t count = 1; count <= 12; ++count) {
      const uint32_t current = ReadingPace::estimateMillis(30000, 100, sample * count, count);
      EXPECT_LE(current > previous ? current - previous : previous - current, 1000U);
      if (count >= 10) EXPECT_EQ(current, sample * 1000);
      previous = current;
    }
  }
}

TEST(ReadingPace, RetainsFractionalSessionMeanAndHandlesLargeTotals) {
  EXPECT_EQ(ReadingPace::sessionAverageMillis(295, 10), 29500U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 100, 295, 10), 29500U);
  EXPECT_EQ(ReadingPace::sessionAverageMillis(UINT32_MAX, UINT16_MAX), 65535000U);
  EXPECT_EQ(ReadingPace::sessionAverageMillis(UINT32_MAX, 0), 0U);
}

TEST_F(BookReadingStatsAtomicTest, ShortSessionReopensAtTheSameBlendedPace) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(30000);
  stats.paceSampleCount = 100;
  const auto prior = stats.averageForwardPageMillis();
  const auto priorCount = stats.paceSampleCount;
  for (uint16_t count = 1; count <= 4; ++count) {
    stats.recordForwardPageRead(40);
    stats.setAverageForwardPageMillis(ReadingPace::estimateMillis(prior, priorCount, 40 * count, count));
  }
  ASSERT_TRUE(stats.save(CACHE_PATH));
  stats = BookReadingStats::load(CACHE_PATH);
  EXPECT_EQ(stats.averageForwardPageMillis(), 34000U);
  EXPECT_EQ(ReadingPace::estimateMillis(stats.averageForwardPageMillis(), stats.paceSampleCount, 0, 0), 34000U);
  stats.setAverageForwardPageMillis(0);
  stats.paceSampleCount = 0;
  EXPECT_EQ(stats.paceFractionMillis, 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(stats.averageForwardPageMillis(), stats.paceSampleCount, 0, 0), 0U);
}

TEST(ReadingPace, LearningSamplesCarryAcrossShortSessions) {
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 1, 30, 1), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 2, 30, 1), 30000U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 1, 60, 2), 30000U);
  // A two-sample prior has less influence than a well-established prior.
  EXPECT_GT(ReadingPace::estimateMillis(30000, 2, 40, 1), ReadingPace::estimateMillis(30000, 100, 40, 1));
}

TEST_F(BookReadingStatsAtomicTest, InvalidFractionTempFallsBackToValidBackup) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".bak").c_str()));
  auto invalid = statsWithSeconds(456);
  invalid.paceFractionMillis = 1000;
  invalid.isCompleted = true;
  ASSERT_TRUE(invalid.save(CACHE_PATH));
  ASSERT_TRUE(Storage.rename(statsPath.c_str(), (statsPath + ".tmp").c_str()));
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
}
