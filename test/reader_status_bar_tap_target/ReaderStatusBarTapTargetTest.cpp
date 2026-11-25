#include <gtest/gtest.h>

#include "ReaderStatusBarTapTarget.h"

TEST(ReaderStatusBarTapTarget, BottomReservesAFingerSizedStripIncludingTheBezelMargin) {
  constexpr int screenHeight = 480;
  constexpr int bottomMargin = 5;

  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(410, screenHeight, bottomMargin, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(411, screenHeight, bottomMargin, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(474, screenHeight, bottomMargin, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(479, screenHeight, bottomMargin, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(480, screenHeight, bottomMargin, 19));
}

TEST(ReaderStatusBarTapTarget, BottomUsesTheFullTallerStatusBar) {
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(395, 480, 5, 80));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(394, 480, 5, 80));
}

TEST(ReaderStatusBarTapTarget, TopUsesTheSameMinimumTouchTarget) {
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(-1, 480, 8, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(0, 480, 8, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(8, 480, 8, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(71, 480, 8, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(72, 480, 8, 19));
}

TEST(ReaderStatusBarTapTarget, RejectsMissingOrInvalidStatusBarRegions) {
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(470, 480, 5, 0));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(8, 480, -1, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(8, 0, 0, 19));
}

TEST(ReaderStatusBarTapTarget, TopAndBottomCanBeTappedIndependently) {
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(16, 480, 8, 24));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(16, 480, 5, 24));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(460, 480, 5, 24));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(460, 480, 8, 24));
}
