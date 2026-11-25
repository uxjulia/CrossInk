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

TEST(HalFrontlight, SuspendingOutputKeepsTheLightSettingAvailableDuringSleep) {
  frontlight_test::detected = true;
  frontlight_test::begun = true;
  Frontlight.begin(73, 42, true);

  Frontlight.setOutputSuspended(true);
  EXPECT_TRUE(Frontlight.isOn());
  EXPECT_EQ(Frontlight.brightness(), 73);
  EXPECT_EQ(frontlight_test::brightness, 0);

  Frontlight.setBrightness(35);
  Frontlight.setWarmth(61);
  Frontlight.setOn(false);
  Frontlight.setOn(true);
  EXPECT_EQ(frontlight_test::brightness, 0);

  Frontlight.setOutputSuspended(false);
  EXPECT_TRUE(Frontlight.isOn());
  EXPECT_EQ(frontlight_test::brightness, 35);
  EXPECT_EQ(frontlight_test::warmth, 61);
}

TEST(HalFrontlight, ResumingOutputKeepsAnUnlitLightOff) {
  frontlight_test::detected = true;
  frontlight_test::begun = true;
  Frontlight.begin(73, 42, false);
  Frontlight.setOutputSuspended(true);
  Frontlight.setOutputSuspended(false);
  EXPECT_FALSE(Frontlight.isOn());
  EXPECT_EQ(frontlight_test::brightness, 0);
}
