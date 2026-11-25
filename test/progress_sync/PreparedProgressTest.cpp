#include <HalStorage.h>
#include <gtest/gtest.h>

#include "sync/PreparedProgress.h"
namespace {
class PreparedProgressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.files.clear();
    failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
    maxBookmarkWriteBytes = shortReadAfter = -1;
    ASSERT_TRUE(PreparedProgress::begin());
  }
  PreparedProgress::Saved saved{1, 2, 32, 40, true};
  KOReaderProgress makeProgress() {
    KOReaderProgress p{};
    p.document.assign(32, 'a');
    p.progress = "/body/DocFragment[2]/body/p[2]/text().4";
    p.percentage = 0.25f;
    p.device = "C3";
    p.metadata.emplace();
    p.metadata->title = "Title";
    p.metadata->seriesIndex = 2.5f;
    return p;
  }
};
TEST_F(PreparedProgressTest, RoundTripAndRejectChangedLocalPosition) {
  auto progress = makeProgress();
  ASSERT_TRUE(PreparedProgress::write("/book.epub", saved, progress));
  KOReaderProgress read{};
  ASSERT_TRUE(PreparedProgress::read("/book.epub", saved, read));
  EXPECT_EQ(read.progress, progress.progress);
  ASSERT_TRUE(read.metadata);
  EXPECT_EQ(read.metadata->seriesIndex, 2.5f);
  ++saved.page;
  EXPECT_FALSE(PreparedProgress::read("/book.epub", saved, read));
}
TEST_F(PreparedProgressTest, DiscardsInterruptedRunAndRejectsTornRecord) {
  ASSERT_TRUE(PreparedProgress::write("/book.epub", saved, makeProgress()));
  for (auto& [path, bytes] : Storage.files)
    if (path.ends_with(".bin") && !bytes.empty()) bytes.pop_back();
  KOReaderProgress read{};
  EXPECT_FALSE(PreparedProgress::read("/book.epub", saved, read));
  ASSERT_TRUE(PreparedProgress::begin());
  EXPECT_FALSE(PreparedProgress::read("/book.epub", saved, read));
}
TEST_F(PreparedProgressTest, DoesNotAcceptFailedStaging) {
  failBookmarkClose = true;
  EXPECT_FALSE(PreparedProgress::write("/book.epub", saved, makeProgress()));
  KOReaderProgress read{};
  EXPECT_FALSE(PreparedProgress::read("/book.epub", saved, read));
}
}  // namespace
