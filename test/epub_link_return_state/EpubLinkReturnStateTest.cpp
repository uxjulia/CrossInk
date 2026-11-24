#include <HalStorage.h>
#include <gtest/gtest.h>

#include "EpubLinkReturnState.h"
#include "ReaderProgressSaveDebouncer.h"

using namespace EpubLinkReturnState;

class EpubLinkReturnStateTest : public testing::Test {
 protected:
  Position positions[MAX_DEPTH] = {};
  int depth = 0;
  void SetUp() override { Storage = {}; }
};

TEST_F(EpubLinkReturnStateTest, FourthJumpKeepsThreeNewestOrigins) {
  for (int i = 1; i <= 4; ++i) push(positions, depth, {i, i * 11});
  ASSERT_EQ(depth, MAX_DEPTH);
  for (int i = 4; i >= 2; --i) {
    const auto pos = positions[--depth];
    EXPECT_EQ(pos.spineIndex, i);
    EXPECT_EQ(pos.pageNumber, i * 11);
  }
}

TEST_F(EpubLinkReturnStateTest, DebouncedDestinationIsIndependentOfPersistedOrigins) {
  // A page read after a jump can still be pending at exit. Its position
  // must stay independent of the saved link origins.
  depth = 0;
  push(positions, depth, {1, 7});
  push(positions, depth, {2, 9});
  ReaderProgressSaveDebouncer progress;
  progress.observe((3U << 16) | 4U, 20);
  progress.markPersisted((3U << 16) | 4U, 20);
  EXPECT_FALSE(progress.observe((3U << 16) | 8U, 20));
  ASSERT_TRUE(progress.hasPending());
  const auto flushedPosition = progress.lastObservedPosition();
  progress.markPersisted(flushedPosition, progress.lastObservedMetadata());
  ASSERT_TRUE(save("book", positions, depth, false));
  EXPECT_EQ(flushedPosition, (3U << 16) | 8U);
  depth = 0;
  ASSERT_TRUE(load("book", positions, depth, 10));
  ASSERT_EQ(depth, 2);
  EXPECT_EQ(positions[--depth].spineIndex, 2);
  EXPECT_EQ(positions[--depth].spineIndex, 1);
  EXPECT_FALSE(Storage.exists("book/links.bin"));
}

TEST_F(EpubLinkReturnStateTest, PreviewResumesImmediateOriginKeepingOlderHistory) {
  push(positions, depth, {1, 7});
  push(positions, depth, {2, 9});
  push(positions, depth, {3, 11});  // The transient preview opens from here.
  const int readingDepth = resumeDepth(depth, true);
  EXPECT_EQ(positions[readingDepth].spineIndex, 3);
  EXPECT_EQ(positions[readingDepth].pageNumber, 11);
  ASSERT_TRUE(save("book", positions, depth, true));
  ASSERT_TRUE(load("book", positions, depth, 10));
  ASSERT_EQ(depth, 2);
  EXPECT_EQ(positions[--depth].spineIndex, 2);
  EXPECT_EQ(positions[--depth].spineIndex, 1);
}

TEST_F(EpubLinkReturnStateTest, EmptyHistoryAndSinglePreviewRemoveStaleRecord) {
  push(positions, depth, {1, 7});
  ASSERT_TRUE(save("book", positions, depth, false));
  ASSERT_TRUE(save("book", positions, depth, true));
  EXPECT_FALSE(Storage.exists("book/links.bin"));
  ASSERT_TRUE(save("book", positions, depth, false));
  ASSERT_TRUE(save("book", positions, 0, false));
  EXPECT_FALSE(Storage.exists("book/links.bin"));
}

TEST_F(EpubLinkReturnStateTest, RoundTripsU16BoundariesAndClosesBeforeConsume) {
  push(positions, depth, {0, 65535});
  push(positions, depth, {65535, 0});
  ASSERT_TRUE(save("book", positions, depth, false));
  EXPECT_EQ(Storage.openHandles, 0);
  ASSERT_TRUE(load("book", positions, depth, 65536));
  ASSERT_EQ(depth, 2);
  EXPECT_EQ(positions[0].pageNumber, 65535);
  EXPECT_EQ(positions[1].spineIndex, 65535);
  EXPECT_EQ(Storage.openHandles, 0);
}

TEST_F(EpubLinkReturnStateTest, RejectsAndConsumesMalformedOrStaleRecords) {
  const std::vector<std::vector<unsigned char>> invalid = {{},
                                                           {0},
                                                           {4},
                                                           {1, 0, 0, 0},
                                                           {1, 0, 0, 0, 0, 99},
                                                           {1, 10, 0, 0, 0},
                                                           {2, 0, 0, 1, 0, 10, 0, 2, 0},
                                                           std::vector<unsigned char>(14, 0)};
  for (const auto& record : invalid) {
    Storage.files["book/links.bin"] = record;
    depth = 3;
    EXPECT_FALSE(load("book", positions, depth, 10));
    EXPECT_EQ(depth, 0);
    EXPECT_FALSE(Storage.exists("book/links.bin"));
    EXPECT_EQ(Storage.openHandles, 0);
  }
}

TEST_F(EpubLinkReturnStateTest, FailedWritesCloseAndRemovePartialStack) {
  push(positions, depth, {1, 7});
  for (int failure = 0; failure < 3; ++failure) {
    Storage.shortWrite = failure == 0;
    Storage.failSync = failure == 1;
    Storage.failClose = failure == 2;
    EXPECT_FALSE(save("book", positions, depth, false));
    EXPECT_EQ(Storage.openHandles, 0);
    EXPECT_FALSE(Storage.exists("book/links.bin"));
  }
}

TEST_F(EpubLinkReturnStateTest, FailedReadsOrConsumptionNeverRestoreHistory) {
  push(positions, depth, {1, 7});
  ASSERT_TRUE(save("book", positions, depth, false));
  Storage.shortRead = true;
  EXPECT_FALSE(load("book", positions, depth, 10));
  EXPECT_EQ(depth, 0);
  Storage.shortRead = false;
  push(positions, depth, {1, 7});
  ASSERT_TRUE(save("book", positions, depth, false));
  Storage.failRemove = true;
  EXPECT_FALSE(load("book", positions, depth, 10));
  EXPECT_EQ(depth, 0);
}

TEST_F(EpubLinkReturnStateTest, ExitUsesRenderedObservationAfterNextPageFails) {
  push(positions, depth, {1, 7});
  ReaderProgressSaveDebouncer progress;
  progress.observe((2U << 16) | 8U, 30);
  // Advancing section->currentPage does not observe it: loading or rendering
  // may fail. The same snapshot policy is used on exit and before KOSync.
  const auto saved = readingProgress(positions, depth, false, progress, 1, 20);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->spineIndex, 2);
  EXPECT_EQ(saved->pageNumber, 8);
  EXPECT_EQ(saved->pageCount, 30);
  progress.markPersisted((2U << 16) | 8U, 30);
  // A previously flushed page is already durable; do not save an unread target.
  EXPECT_FALSE(readingProgress(positions, depth, false, progress, 2, 30));
}

TEST_F(EpubLinkReturnStateTest, PreviewResumeUsesImmediateRenderedOriginAndItsMetadata) {
  push(positions, depth, {1, 7});
  push(positions, depth, {2, 9});
  ReaderProgressSaveDebouncer progress;
  progress.observe((2U << 16) | 9U, 30);
  const auto saved = readingProgress(positions, depth, true, progress, 1, 100);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->spineIndex, 2);
  EXPECT_EQ(saved->pageNumber, 9);
  EXPECT_EQ(saved->pageCount, 30);
  // Preview pages never become normal observations. Older full-section history
  // survives the preview's dismissal and cannot replace this reading origin.
  ASSERT_TRUE(save("book", positions, depth, true));
  ASSERT_TRUE(load("book", positions, depth, 10));
  ASSERT_EQ(depth, 1);
  EXPECT_EQ(positions[0].spineIndex, 1);
}

TEST_F(EpubLinkReturnStateTest, PreviewMetadataFallbackNeverUsesAnotherSpinesCount) {
  push(positions, depth, {2, 9});
  ReaderProgressSaveDebouncer progress;
  progress.observe((1U << 16) | 7U, 100);
  auto saved = readingProgress(positions, depth, true, progress, 2, 30);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->pageCount, 30);
  saved = readingProgress(positions, depth, true, progress, 1, 100);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->pageCount, 0);
}

TEST_F(EpubLinkReturnStateTest, ReplacementFailurePreservesPreviousCheckpointBytes) {
  push(positions, depth, {1, 7});
  ASSERT_TRUE(save("book", positions, depth, false));
  const auto original = Storage.files.at("book/links.bin");
  push(positions, depth, {2, 8});
  for (int failure = 0; failure < 3; ++failure) {
    Storage.shortWrite = failure == 0;
    Storage.failSync = failure == 1;
    Storage.failClose = failure == 2;
    EXPECT_FALSE(save("book", positions, depth, false));
    EXPECT_EQ(original, Storage.files.at("book/links.bin"));
    EXPECT_EQ(Storage.openHandles, 0);
  }
  Storage.failClose = false;
  ASSERT_TRUE(Storage.rename("book/links.bin", "book/links.bin.bak"));
  Storage.files["book/links.bin.tmp"] = {2};
  ASSERT_TRUE(load("book", positions, depth, 10));
  ASSERT_EQ(depth, 1);
  EXPECT_EQ(positions[0].pageNumber, 7);
}
