#include <gtest/gtest.h>

#include "util/InputReleaseGuard.h"

namespace {

class InputReleaseGuardTest : public testing::TestWithParam<MappedInputManager::Button> {};

TEST_P(InputReleaseGuardTest, HeldLaunchConsumesOnlyItsRelease) {
  MappedInputManager input;
  const auto button = GetParam();
  bool pending = true;

  input.setPressed(button, true);
  EXPECT_TRUE(InputReleaseGuard::consumeInitialRelease(input, button, pending));
  EXPECT_TRUE(pending);

  input.setPressed(button, false);
  input.setReleased(button, true);
  EXPECT_TRUE(InputReleaseGuard::consumeInitialRelease(input, button, pending));
  EXPECT_FALSE(pending);

  // A later physical press/release belongs to the destination activity.
  EXPECT_FALSE(InputReleaseGuard::consumeInitialRelease(input, button, pending));
}

TEST_P(InputReleaseGuardTest, ReleaseFirstLaunchLeavesTheNextReleaseUntouched) {
  MappedInputManager input;
  const auto button = GetParam();
  bool pending = input.isPressed(button);

  EXPECT_FALSE(pending);
  input.setReleased(button, true);
  EXPECT_FALSE(InputReleaseGuard::consumeInitialRelease(input, button, pending));
}

INSTANTIATE_TEST_SUITE_P(ShortcutButtons, InputReleaseGuardTest,
                         testing::Values(MappedInputManager::Button::Back, MappedInputManager::Button::Confirm,
                                         MappedInputManager::Button::Power, MappedInputManager::Button::Up,
                                         MappedInputManager::Button::Down));

}  // namespace
