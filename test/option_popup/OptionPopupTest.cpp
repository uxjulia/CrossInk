#include <gtest/gtest.h>

#include <functional>

#include "components/OptionPopup.h"

namespace {

class PopupTouchHarness {
 public:
  explicit PopupTouchHarness(MappedInputManager& input) : input(input) {}

  void touchPopup(OptionPopup& popup, const int optionY, const std::function<void()>& requestUpdate) {
    input.injectTouchDown(400, optionY);
    ASSERT_TRUE(popup.handleInput(input, requestUpdate));
    input.injectTouchRelease(400, optionY);
    ASSERT_TRUE(popup.handleInput(input, requestUpdate));
  }

 private:
  MappedInputManager& input;
};

TEST(OptionPopup, ConsecutiveTouchSelectionsDoNotLoseSecondRelease) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  PopupTouchHarness touch(input);
  int firstSelections = 0;
  int secondSelections = 0;

  const char* firstOptions[] = {"First"};
  const char* secondOptions[] = {"Second"};
  popup.show("First popup", firstOptions, 1, 0, [&](const int selectedIndex) {
    EXPECT_EQ(selectedIndex, 0);
    ++firstSelections;
    // Quick Actions immediately reuses the popup for the next choice.
    popup.show("Second popup", secondOptions, 1, 0, [&](const int nextIndex) {
      EXPECT_EQ(nextIndex, 0);
      ++secondSelections;
    });
  });

  touch.touchPopup(popup, 250, [] {});
  EXPECT_EQ(firstSelections, 1);
  EXPECT_EQ(secondSelections, 0);
  ASSERT_TRUE(popup.isActive());

  touch.touchPopup(popup, 250, [] {});
  EXPECT_EQ(firstSelections, 1);
  EXPECT_EQ(secondSelections, 1);
  EXPECT_FALSE(popup.isActive());
}

TEST(OptionPopup, PowerConfirmSelectionSuppressesItsPowerRelease) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  int selections = 0;

  const char* options[] = {"Toggle Frontlight"};
  popup.show("Quick Actions", options, 1, 0, [&](const int selectedIndex) {
    EXPECT_EQ(selectedIndex, 0);
    ++selections;
  });

  input.injectPowerConfirmPress();
  EXPECT_TRUE(popup.handleInput(input, [] {}));

  EXPECT_EQ(selections, 1);
  EXPECT_FALSE(popup.isActive());
  EXPECT_TRUE(input.isPowerReleaseSuppressed());

  input.injectPowerConfirmRelease();
  EXPECT_FALSE(input.wasReleased(MappedInputManager::Button::Power));
  // The global dispatcher and active reader can both observe the same
  // hardware release edge in one loop; neither may run the shortcut.
  EXPECT_FALSE(input.wasReleased(MappedInputManager::Button::Power));

  input.advanceInputFrame();
  input.injectPowerConfirmPress();
  input.injectPowerConfirmRelease();
  EXPECT_TRUE(input.wasReleased(MappedInputManager::Button::Power));
}

TEST(OptionPopup, DisabledTouchOptionDoesNotSelect) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  PopupTouchHarness touch(input);
  int selections = 0;

  const char* options[] = {"Unavailable"};
  popup.show("Sync & Transfer", options, 1, 0, [&](const int) { ++selections; });
  popup.setDisabledOptions({true});

  touch.touchPopup(popup, 250, [] {});
  EXPECT_EQ(selections, 0);
  EXPECT_TRUE(popup.isActive());
}

TEST(OptionPopup, ButtonNavigationSkipsDisabledOptions) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  int selectedIndex = -1;

  const char* options[] = {"Sync Progress", "Nearby Position Sync", "Send to Nearby Device"};
  popup.show("Sync & Transfer", options, 3, 2, [&](const int index) { selectedIndex = index; });
  popup.setDisabledOptions({true, true, false});

  ButtonNavigator::injectNextRelease();
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  input.injectPowerConfirmPress();
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  EXPECT_EQ(selectedIndex, 2);
}

}  // namespace
