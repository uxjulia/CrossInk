#include <gtest/gtest.h>

#include <limits>

#include "StableReadingPace.h"

TEST(StableReadingPace, LearnsOnlyFromThreeValidForwardReads) {
  StableReadingPace pace;
  uint32_t seconds = 99;
  pace.record(0, 1000);
  pace.record(30, 0);
  pace.record(30, -1000);
  pace.record(30, std::numeric_limits<double>::quiet_NaN());
  pace.record(30, 1000);
  pace.record(30, 1000);
  EXPECT_FALSE(pace.estimate(100000, seconds));
  EXPECT_EQ(seconds, 0U);
  pace.record(30, 1000);
  ASSERT_TRUE(pace.estimate(100000, seconds));
  EXPECT_EQ(seconds, 3000U);
}

TEST(StableReadingPace, SteadyTextSpeedAlwaysCountsDownDespiteDifferentScreenSizes) {
  StableReadingPace pace;
  for (int i = 0; i < 3; ++i) pace.record(30, 1000);
  double remaining = 100000;
  uint32_t previous = 0;
  ASSERT_TRUE(pace.estimate(remaining, previous));
  for (uint32_t duration : {30U, 60U, 15U, 90U, 30U, 15U}) {
    const double distance = duration * 1000.0 / 30;
    remaining -= distance;
    pace.record(duration, distance);
    uint32_t seconds = 0;
    ASSERT_TRUE(pace.estimate(remaining, seconds));
    EXPECT_EQ(seconds, previous - duration);
    previous = seconds;
  }
}

TEST(StableReadingPace, KeepsFractionalStablePageProgressAndAdaptsToRealPaceChanges) {
  StableReadingPace pace;
  for (int i = 0; i < 3; ++i) pace.record(30, 0.25);
  uint32_t seconds = 0;
  ASSERT_TRUE(pace.estimate(10.5, seconds));
  EXPECT_EQ(seconds, 1260U);
  pace.record(60, 0.25);
  ASSERT_TRUE(pace.estimate(10.25, seconds));
  EXPECT_EQ(seconds, 1538U);
}

TEST(StableReadingPace, ResetAndExtremeValuesAreSafe) {
  StableReadingPace pace;
  for (int i = 0; i < 3; ++i) pace.record(30, 1);
  uint32_t seconds = 0;
  EXPECT_FALSE(pace.estimate(0, seconds));
  EXPECT_FALSE(pace.estimate(std::numeric_limits<double>::infinity(), seconds));
  ASSERT_TRUE(pace.estimate(UINT32_MAX, seconds));
  EXPECT_EQ(seconds, UINT32_MAX);
  pace = {};
  EXPECT_FALSE(pace.estimate(100, seconds));
}
