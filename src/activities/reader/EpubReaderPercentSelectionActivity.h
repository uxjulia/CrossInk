#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public Activity {
 public:
#ifdef SIMULATOR
  friend struct SliderOrientationSmokeTest;
#endif
  // Slider-style percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              float initialPercent);
  EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t initialPage,
                                     uint32_t pageCount);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  // FreeInkApp hosts the 4x3 numeric keypad (touch always; non-touch once the user
  // holds Confirm to enter it) and its backspace icon. The shared CrossInk back
  // header stays separate. 12 grid keys + 1 backspace, one spare slot; 2 handlers.
  using UiApp = freeink::ui::FreeInkApp<14, 2>;

  static void percentScreen(UiApp::ScreenType& screen, void* user);
  static void onKeypadKeyEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onKeypadBackspaceEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildPercentScreen(UiApp::ScreenType& screen);
  void buildKeypadScreen(UiApp::ScreenType& screen, char* line, size_t lineSize);
  void cancel();
  void confirm();

  enum class Mode : uint8_t { Percent, StablePage };
  Mode mode = Mode::Percent;
  // Percent mode: centipercent (0-10000, i.e. hundredths of a percent).
  // StablePage mode: the page number directly (0-maximum).
  uint32_t value = 0;
  uint32_t maximum = 100;

  ButtonNavigator buttonNavigator;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};

  // Change the current value by a delta (page count, or centipercent) and wrap within bounds.
  void adjustValue(int delta);
  // Maps a display step (1 or 10, same units shown in the front/side button hints)
  // to the actual delta for the current mode's storage unit.
  int deltaForDisplayStep(int displaySteps) const;

  // Numeric keypad entry, alongside the slider. Touch always shows the keypad;
  // non-touch defaults to the slider and enters keypad entry via long-press Confirm.
  bool isKeypadVisible() const;
  void enterKeypad();
  void exitKeypad();
  void seedKeypadEntryFromValue();
  void moveKeypadFocus(int rowDelta, int colDelta);
  void activateKeypadFocus();
  void handleKeypadValue(int16_t keyValue);
  void appendDigit(char digit);
  void appendDecimalPoint();
  void backspaceEntry();
  // Parses the typed digits (if any) into `value`, then finishes like confirm().
  void confirmKeypad();

  bool keypadActive = false;
  char entryText[12] = {0};
  uint8_t entryLen = 0;
  int keypadRow = 0;
  int keypadCol = 0;
  bool keypadBackspaceFocused = false;
  // Long-press Confirm toggles into keypad mode (or backspaces within it); swallow
  // the eventual release so it doesn't also fire the short-press action.
  bool confirmLongPressFired = false;
};
