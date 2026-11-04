#include <HalFrontlight.h>
#include <gtest/gtest.h>

TEST(HalFrontlight, ProbesBeforeCheckingI2cPresenceAndRestoresSavedLevels) {
  frontlight_test::begun = false;
  frontlight_test::detected = true;
  Frontlight.begin(73, 42, true);
  EXPECT_TRUE(Frontlight.present());
  EXPECT_EQ(frontlight_test::brightness, 73);
  EXPECT_EQ(frontlight_test::warmth, 42);
  Frontlight.setOn(false);
  EXPECT_EQ(frontlight_test::brightness, 0);
  Frontlight.setOn(true);
  EXPECT_EQ(frontlight_test::brightness, 73);
}

TEST(HalFrontlight, AbsentControllerDoesNotApplyBrightness) {
  frontlight_test::begun = false;
  frontlight_test::detected = false;
  frontlight_test::brightness = 0;
  Frontlight.begin(73, 42, true);
  EXPECT_FALSE(Frontlight.present());
  EXPECT_EQ(frontlight_test::brightness, 0);
}
