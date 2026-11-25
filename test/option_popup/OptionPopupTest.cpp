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

  popup.render(renderer);
  touch.touchPopup(popup, popup.simulatorOptionRect(0).y + 10, [] {});
  EXPECT_EQ(firstSelections, 1);
  EXPECT_EQ(secondSelections, 0);
  ASSERT_TRUE(popup.isActive());

  popup.render(renderer);
  touch.touchPopup(popup, popup.simulatorOptionRect(0).y + 10, [] {});
  EXPECT_EQ(firstSelections, 1);
  EXPECT_EQ(secondSelections, 1);
  EXPECT_FALSE(popup.isActive());
}

TEST(OptionPopup, MenuAndSortDecorationsDoNotLeakToNextPopup) {
  GfxRenderer renderer;
  OptionPopup popup;
  const char* options[] = {"First", "Second"};

  popup.show("Sort", options, 2, 0, [](const int) {});
  popup.setDividerAfterOption(0);
  popup.render(renderer);
  EXPECT_EQ(renderer.lineCount(), 1);
  EXPECT_EQ(renderer.triangleCount(), 0);

  popup.show("Menu", options, 2, 0, [](const int) {});
  popup.setSelectionArrow(true);
  popup.render(renderer);
  EXPECT_EQ(renderer.lineCount(), 1);
  EXPECT_EQ(renderer.triangleCount(), 1);

  popup.show("Other", options, 2, 0, [](const int) {});
  popup.render(renderer);
  EXPECT_EQ(renderer.lineCount(), 1);
  EXPECT_EQ(renderer.triangleCount(), 1);
}

TEST(OptionPopup, NotesFollowHighlightAndResetOnReuse) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  const char* options[] = {"Directional", "Legacy"};
  popup.show("Navigation", options, 2, 0, [](const int) {});
  popup.setOptionNotes({{"Front:", "Up/Down", "Side:", "Left/Right"}, {"Front:", "Up/Down", "Side:", "Up/Down"}});
  popup.render(renderer);
  EXPECT_EQ(GUI.lastNoteLabel, "Front:");
  EXPECT_EQ(GUI.lastNoteBody, "Up/Down");
  EXPECT_EQ(GUI.lastSecondNoteLabel, "Side:");
  EXPECT_EQ(GUI.lastSecondNoteBody, "Left/Right");

  ButtonNavigator::injectNextRelease();
  popup.handleInput(input, [] {});
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 1);
  EXPECT_EQ(GUI.lastSecondNoteBody, "Up/Down");

  popup.show("Other", options, 2, 0, [](const int) {}, {"Note:", "Existing note"});
  popup.render(renderer);
  EXPECT_EQ(GUI.lastNoteLabel, "Note:");
  EXPECT_EQ(GUI.lastNoteBody, "Existing note");
  EXPECT_TRUE(GUI.lastSecondNoteLabel.empty());
  EXPECT_TRUE(GUI.lastSecondNoteBody.empty());
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

  popup.render(renderer);
  touch.touchPopup(popup, popup.simulatorOptionRect(0).y + 10, [] {});
  EXPECT_EQ(selections, 0);
  EXPECT_TRUE(popup.isActive());
}

TEST(OptionPopup, InertRowIgnoresTouchAndConfirmAndResetsOnReuse) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  PopupTouchHarness touch(input);
  int selections = 0;
  const char* options[] = {"English: Built in"};
  popup.show("Packs", options, 1, 0, [&](int) { ++selections; });
  popup.setInertOptionIndex(0);
  popup.render(renderer);
  touch.touchPopup(popup, popup.simulatorOptionRect(0).y + 10, [] {});
  EXPECT_TRUE(popup.isActive());
  EXPECT_EQ(selections, 0);
  input.injectPowerConfirmPress();
  popup.handleInput(input, [] {});
  EXPECT_TRUE(popup.isActive());
  EXPECT_EQ(selections, 0);
  input.injectPowerConfirmRelease();
  input.advanceInputFrame();
  popup.show("Other", options, 1, 0, [&](int) { ++selections; });
  popup.render(renderer);
  touch.touchPopup(popup, popup.simulatorOptionRect(0).y + 10, [] {});
  EXPECT_EQ(selections, 1);
  EXPECT_FALSE(popup.isActive());
}

TEST(OptionPopup, InertTouchPreservesAnotherRowsHighlightAndConfirmAction) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  int selected = -1;
  const char* options[] = {"English: Built in", "German: Install"};
  popup.show("Packs", options, 2, 1, [&](int value) { selected = value; });
  popup.setInertOptionIndex(0);
  popup.render(renderer);
  EXPECT_FALSE(popup.simulatorOptionDisabled(0));
  const auto row = popup.simulatorOptionRect(0);
  input.injectTouchDown(row.x + row.width / 2, row.y + row.height / 2);
  popup.handleInput(input, [] {});
  input.injectTouchRelease(row.x + row.width / 2, row.y + row.height / 2);
  popup.handleInput(input, [] {});
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 1);
  EXPECT_EQ(selected, -1);
  input.injectPowerConfirmPress();
  popup.handleInput(input, [] {});
  EXPECT_EQ(selected, 1);
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

TEST(OptionPopup, SwipeDoesNotHighlightTheTouchDownOption) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;

  std::vector<std::string> options;
  for (int i = 0; i < 25; ++i) options.push_back(std::to_string(i));
  popup.show("Long popup", options, 0, [](const int) {});

  // The second visible row is a tentative touch target. Do not redraw it as
  // selected until the contact completes as a tap rather than a swipe.
  input.injectTouchDown(400, 116);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 0);

  input.injectSwipe(MappedInputManager::SwipeDir::Up);
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 0);
  EXPECT_EQ(GUI.getLastFirstOptionIndex(), 10);
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

TEST(OptionPopup, CalibratedRowsStayVisibleAndReceiveTheirOwnTouches) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  const std::array<int, 4> cases[] = {{9, 3, 3, 3}, {0, 0, 0, 0}, {32, 32, 32, 32}, {21, 7, 32, 13}};
  for (const auto& edges : cases) {
    renderer.insets = edges;
    OptionPopup popup;
    int selected = -1;
    const char* options[] = {"One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight"};
    popup.show("Calibration options", options, 8, 0, [&](int index) { selected = index; });
    popup.render(renderer);
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    for (int i = 0; i < 8; ++i) {
      const auto row = popup.simulatorOptionRect(i);
      EXPECT_GE(row.x, safe.x);
      EXPECT_GE(row.y, safe.y);
      EXPECT_LE(row.x + row.width, safe.x + safe.width);
      EXPECT_LE(row.y + row.height, safe.y + safe.height);
    }
    const auto row = popup.simulatorOptionRect(7);
    const int x = row.x + row.width / 2;
    const int y = row.y + row.height / 2;
    input.injectTouchDown(x, y);
    popup.handleInput(input, [] {});
    input.injectTouchRelease(x, y);
    popup.handleInput(input, [] {});
    EXPECT_EQ(selected, 7);
    input.advanceInputFrame();
  }
}

}  // namespace

TEST(OptionPopup, BorrowedDisabledOptionsAndOwnedReuse) {
  GfxRenderer renderer;
  HalGPIO gpio;
  MappedInputManager input(gpio, renderer);
  OptionPopup popup;
  std::vector<std::string> names{"English", "Duplicate", "Community"};
  std::vector<bool> disabled{false, true, false};
  int selected = -1;
  popup.showBorrowed(STR_SAVE, OptionLabels(names, disabled), 0, [&](int index) { selected = index; });
  ButtonNavigator::injectNextRelease();
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 2);
  input.injectPowerConfirmPress();
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  EXPECT_EQ(selected, 2);
  popup.clear();
  names.clear();
  disabled.clear();
  const char* owned[] = {"Owned"};
  popup.show("Reuse", owned, 1, 0, [&](int index) { selected = index + 10; });
  popup.render(renderer);
  EXPECT_EQ(GUI.getLastSelectedIndex(), 0);
  input.injectPowerConfirmPress();
  EXPECT_TRUE(popup.handleInput(input, [] {}));
  EXPECT_EQ(selected, 10);
}
