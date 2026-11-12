#include <CalibreUploadProgress.h>
#include <gtest/gtest.h>

TEST(CalibreProgress, RepaintsLatestByteCountAfterIntervalEvenIfTransferPauses) {
  CalibreUploadProgress progress;
  constexpr uint32_t start = UINT32_MAX - 1500;
  EXPECT_TRUE(progress.update(true, "book", 0, 100, start));
  EXPECT_FALSE(progress.update(true, "book", 10, 100, start + 10));
  EXPECT_FALSE(progress.update(true, "book", 30, 100, start + 3999));
  EXPECT_TRUE(progress.update(true, "book", 30, 100, start + 4000));
  EXPECT_EQ(progress.received, 30u);
  EXPECT_FALSE(progress.update(true, "book", 30, 100, start + 8000));
}

TEST(CalibreProgress, SwitchFinishAndRestartRepaintImmediately) {
  CalibreUploadProgress progress;
  EXPECT_FALSE(progress.update(false, "", 0, 0, 0));
  EXPECT_TRUE(progress.update(true, "A", 100, 1000, 1));
  EXPECT_TRUE(progress.update(true, "B", 100, 1000, 2));
  EXPECT_TRUE(progress.update(true, "B", 0, 1000, 3));
  EXPECT_TRUE(progress.update(false, "B", 1000, 1000, 4));
  EXPECT_EQ(progress.total, 0u);
  EXPECT_TRUE(progress.filename.empty());
  EXPECT_FALSE(progress.update(false, "B", 1000, 1000, 5));
  EXPECT_TRUE(progress.update(true, "B", 0, 1000, 6));
}
