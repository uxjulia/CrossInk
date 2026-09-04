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

TEST(OptionPopup, OutsideTouchDismissesWhenEnabled) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;

  const char* options[] = {"Action"};
  popup.show("Image actions", options, 1, 0, [](const int) {});
  popup.setDismissOnOutsideTouchDown(true);

  input.injectTouchDown(0, 0);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  EXPECT_FALSE(popup.isActive());

  input.injectTouchRelease(0, 0);
  int touchX = 0;
  int touchY = 0;
  EXPECT_FALSE(input.wasScreenTapped(touchX, touchY));
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

TEST(OptionPopup, SwipeMovesByAVisiblePage) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  PopupTouchHarness touch(input);
  int selectedIndex = -1;

  std::vector<std::string> options;
  for (int i = 0; i < 25; ++i) options.push_back(std::to_string(i));
  popup.show("Long popup", options, 0, [&](const int index) { selectedIndex = index; });

  input.injectSwipe(MappedInputManager::SwipeDir::Up);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastFirstOptionIndex(), 10);
  touch.touchPopup(popup, 80, [] {});
  EXPECT_EQ(selectedIndex, 10);

  popup.show("Long popup", options, 24, [&](const int index) { selectedIndex = index; });
  input.injectSwipe(MappedInputManager::SwipeDir::Down);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastFirstOptionIndex(), 5);
  touch.touchPopup(popup, 80, [] {});
  EXPECT_EQ(selectedIndex, 5);
}

TEST(OptionPopup, SwipeKeepsDisabledDestinationUnselectableWithoutWrapping) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  PopupTouchHarness touch(input);
  int selectedIndex = -1;

  std::vector<std::string> options;
  for (int i = 0; i < 25; ++i) options.push_back(std::to_string(i));
  popup.show("Long popup", options, 0, [&](const int index) { selectedIndex = index; });
  std::vector<bool> disabled(25, false);
  disabled[10] = true;
  popup.setDisabledOptions(std::move(disabled));

  input.injectSwipe(MappedInputManager::SwipeDir::Up);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  touch.touchPopup(popup, 80, [] {});
  EXPECT_EQ(selectedIndex, -1);
  EXPECT_TRUE(popup.isActive());

  touch.touchPopup(popup, 116, [] {});
  EXPECT_EQ(selectedIndex, 11);
}

}  // namespace
