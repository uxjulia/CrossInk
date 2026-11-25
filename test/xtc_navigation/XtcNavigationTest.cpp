#include <gtest/gtest.h>

#include <limits>

#include "XtcReaderNavigation.h"

TEST(XtcNavigation, PercentageMatchesOneBasedProgressInLongBooks) {
  EXPECT_EQ(xtcPageForPercent(50.0f, 10000), 4999u);
  EXPECT_EQ(xtcPageForPercent(12.34f, 10000), 1233u);
  EXPECT_EQ(xtcPageForPercent(25.0f, 5000), 1249u);
  EXPECT_EQ(xtcPageForPercent(50.0f, 3), 1u);
}

TEST(XtcNavigation, EndpointsStayOnValidPages) {
  for (const uint32_t count : {1u, 3u, 5000u, 10000u, 65535u}) {
    EXPECT_EQ(xtcPageForPercent(0.0f, count), 0u);
    EXPECT_EQ(xtcPageForPercent(-1.0f, count), 0u);
    EXPECT_EQ(xtcPageForPercent(100.0f, count), count - 1);
    EXPECT_EQ(xtcPageForPercent(101.0f, count), count - 1);
  }
  EXPECT_EQ(xtcPageForPercent(50.0f, 0), 0u);
  EXPECT_EQ(xtcPageForPercent(std::numeric_limits<float>::quiet_NaN(), 10000), 0u);
}

TEST(XtcNavigation, HundredthPercentStepsRemainOrderedAndBounded) {
  for (const uint32_t count : {1u, 3u, 5000u, 10000u, 65535u}) {
    uint32_t previous = 0;
    for (int i = 0; i <= 10000; ++i) {
      const auto page = xtcPageForPercent(i / 100.0f, count);
      EXPECT_GE(page, previous);
      EXPECT_LT(page, count);
      previous = page;
    }
  }
}
