#include <gtest/gtest.h>

#include "src/activities/util/IntervalSelectionLayout.h"

TEST(IntervalSelectionLayout, ControlsClearHeadersAndFootersAcrossDeviceSizes) {
  // Landscape/portrait heights, calibrated header offsets, both UI scales,
  // touch step controls, button hints, and bottom-anchored reader previews.
  for (const int screenHeight : {320, 480, 528, 552, 768, 800}) {
    for (const int headerBottom : {45, 90, 120, 180}) {
      for (const int valueHeight : {28, 36, 44}) {
        for (const bool touch : {false, true}) {
          for (const bool preview : {false, true}) {
            const int top = headerBottom + 12;
            const int bottom = screenHeight - (touch ? 152 : 40);
            const int stepsHeight = touch ? 56 : 48;
            const auto layout = intervalSelectionLayout(top, bottom, valueHeight, stepsHeight, 12, preview);
            SCOPED_TRACE(::testing::Message() << screenHeight << '/' << headerBottom << '/' << valueHeight << '/'
                                              << touch << '/' << preview);
            if (!layout.visible) {
              EXPECT_LT(bottom - top, valueHeight + 12 + 24);
              EXPECT_FALSE(layout.showSteps);
              continue;
            }
            EXPECT_GE(layout.valueY, top);
            EXPECT_GE(layout.barY - 4, layout.valueY + valueHeight + 12);
            EXPECT_LE(layout.barY + layout.barHeight + 4, bottom);
            if (layout.showSteps) {
              EXPECT_GE(layout.stepsY, layout.barY + layout.barHeight + 4 + 12);
              EXPECT_LE(layout.stepsY + stepsHeight, bottom);
            }
          }
        }
      }
    }
  }
}

TEST(IntervalSelectionLayout, ShortScreensHideOptionalStepsBeforeOverlapping) {
  const auto layout = intervalSelectionLayout(120, 200, 36, 56, 12, false);
  ASSERT_TRUE(layout.visible);
  EXPECT_FALSE(layout.showSteps);
  EXPECT_GE(layout.valueY, 120);
  EXPECT_LE(layout.barY + layout.barHeight + 4, 200);
  EXPECT_FALSE(intervalSelectionLayout(120, 180, 36, 56, 12, false).visible);
  EXPECT_FALSE(intervalSelectionLayout(120, 100, 36, 56, 12, false).visible);
}

TEST(IntervalSelectionLayout, ReaderPreviewControlsStayAnchoredToTheBottom) {
  const auto layout = intervalSelectionLayout(100, 700, 36, 48, 12, true);
  ASSERT_TRUE(layout.visible);
  ASSERT_TRUE(layout.showSteps);
  EXPECT_EQ(layout.stepsY + 48, 700);
  EXPECT_GT(layout.valueY, 100);
}
