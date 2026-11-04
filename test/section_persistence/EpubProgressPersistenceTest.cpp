#include <gtest/gtest.h>

#include "activities/reader/EpubReaderUtils.h"

namespace {
class EpubProgressPersistenceTest : public testing::Test {
 protected:
  Epub epub{"/books/test.epub", "/cache"};
  void SetUp() override { Storage.reset(); }
};

TEST_F(EpubProgressPersistenceTest, IdenticalSavesDoNotWriteOrRotateTheBackup) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10, 456));
  const auto backup = Storage.bytes("/cache/progress.bin.bak");
  const auto writes = Storage.writeOpens;
  const auto removes = Storage.removes;
  const auto renames = Storage.renames;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10, 456));
  EXPECT_EQ(Storage.writeOpens, writes);
  EXPECT_EQ(Storage.removes, removes);
  EXPECT_EQ(Storage.renames, renames);
  EXPECT_EQ(Storage.bytes("/cache/progress.bin.bak"), backup);
}

TEST_F(EpubProgressPersistenceTest, EveryPersistedFieldAndOptionalOffsetChangesTheSave) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  EXPECT_EQ(Storage.writeOpens, 1U);
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 2, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 3, 11));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 3, 11, 0));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 3, 11, 500));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 2, 3, 11));
  EXPECT_EQ(Storage.writeOpens, 7U);
  EpubReaderUtils::Progress progress;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, progress));
  EXPECT_EQ(progress.spineIndex, 2);
  EXPECT_EQ(progress.pageNumber, 3);
  EXPECT_EQ(progress.pageCount, 11);
  EXPECT_FALSE(progress.hasVisibleTextOffset);
}

TEST_F(EpubProgressPersistenceTest, MissingOrTruncatedFilesAreRecreated) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  Storage.remove("/cache/progress.bin");
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  Storage.put("/cache/progress.bin", {1, 0, 2, 0});
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  EXPECT_EQ(Storage.writeOpens, 3U);
  EXPECT_EQ(Storage.bytes("/cache/progress.bin.bak").size(), 20U);
  EpubReaderUtils::Progress progress;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, progress));
  EXPECT_EQ(progress.visibleTextOffset, 123U);
}

TEST_F(EpubProgressPersistenceTest, FailedWritesKeepTheOldPositionAndRemainRetryable) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  const auto original = Storage.bytes("/cache/progress.bin");
  Storage.failWrite = true;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  EXPECT_EQ(Storage.bytes("/cache/progress.bin"), original);
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  EpubReaderUtils::Progress progress;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, progress));
  EXPECT_EQ(progress.pageNumber, 3);
}

TEST_F(EpubProgressPersistenceTest, SavesDoNotDependOnRenameOrRemove) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  Storage.failRenameFrom = "/cache/progress.bin.tmp";
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 4, 10));
  EXPECT_EQ(Storage.renames, 0U);
  EXPECT_EQ(Storage.removes, 0U);
  EpubReaderUtils::Progress progress;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, progress));
  EXPECT_EQ(progress.pageNumber, 4);
}
}  // namespace
