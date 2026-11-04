#include <gtest/gtest.h>

#include "src/util/SlideSwitchToggle.h"

TEST(SlideSwitchToggle, BootAtEitherPositionDoesNotToggle) {
  for (const bool position : {false, true}) {
    SlideSwitchToggle toggle;
    EXPECT_FALSE(toggle.update(position));
    EXPECT_FALSE(toggle.update(position));
  }
}

TEST(SlideSwitchToggle, EachMovementInEitherDirectionTogglesOnce) {
  SlideSwitchToggle toggle;
  EXPECT_FALSE(toggle.update(false));
  EXPECT_TRUE(toggle.update(true));
  EXPECT_FALSE(toggle.update(true));
  EXPECT_TRUE(toggle.update(false));
  EXPECT_FALSE(toggle.update(false));
}

TEST(SlideSwitchToggle, FirmwareChangeIsPreservedUntilNextMovement) {
  SlideSwitchToggle toggle;
  bool enabled = false;
  toggle.update(false);
  enabled = true;  // Firmware enables the function with the switch still down.
  if (toggle.update(false)) enabled = !enabled;
  EXPECT_TRUE(enabled);
  if (toggle.update(true)) enabled = !enabled;
  EXPECT_FALSE(enabled);
  if (toggle.update(false)) enabled = !enabled;
  EXPECT_TRUE(enabled);
}

TEST(SlideSwitchToggle, WakeOrRestartUsesNewPositionOnlyAsBaseline) {
  for (const bool position : {false, true}) {
    SlideSwitchToggle beforeSleep;
    beforeSleep.update(!position);
    SlideSwitchToggle afterWake;
    EXPECT_FALSE(afterWake.update(position));
    EXPECT_FALSE(afterWake.update(position));
    EXPECT_TRUE(afterWake.update(!position));
  }
}

TEST(SlideSwitchToggle, RebindingWithoutMovementDoesNotToggle) {
  SlideSwitchToggle toggle;
  toggle.update(true);
  EXPECT_FALSE(toggle.update(true));
  // A different assigned action observes the same tracker and position.
  EXPECT_FALSE(toggle.update(true));
  EXPECT_TRUE(toggle.update(false));
}
