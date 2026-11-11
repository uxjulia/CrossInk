#include "EpubReaderPercentSelectionActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "StablePageSelectionModel.h"
#include "components/ReaderSliderHints.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/keyboardIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_KEYPAD_KEY = 1;
constexpr fui::ActionId ACTION_KEYPAD_BACKSPACE = 2;
// Display-only step sizes shown in the front/side button hints (1 and 10, in
// whichever unit the current mode reads in). Actual deltas applied to `value`
// go through deltaForDisplayStep() since Percent mode stores centipercent.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
constexpr unsigned long kLongPressMs = 1000;
// Sentinel key values for the two non-digit keypad keys; digits use their own 0-9 value.
constexpr int16_t kKeypadDot = -1;
constexpr int16_t kKeypadOk = -2;
}  // namespace

EpubReaderPercentSelectionActivity::EpubReaderPercentSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const float initialPercent)
    : Activity("EpubReaderPercentSelection", renderer, mappedInput),
      // Round to the nearest whole percent, not the nearest centipercent: every slider
      // step is a multiple of 1.00%, so seeding a fractional remainder here would make
      // 0% and 100% permanently unreachable by stepping (only the keypad could type them).
      value(static_cast<uint32_t>(std::lround(std::clamp(initialPercent, 0.0f, 100.0f))) * 100),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

EpubReaderPercentSelectionActivity::EpubReaderPercentSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const uint32_t initialPage,
                                                                       const uint32_t pageCount)
    : Activity("EpubReaderStablePageSelection", renderer, mappedInput),
      mode(Mode::StablePage),
      value(clampStablePage(initialPage, pageCount)),
      maximum(pageCount),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  ReaderSliderHints::bindFont(uiTarget);
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_KEYPAD_KEY, &EpubReaderPercentSelectionActivity::onKeypadKeyEvent, this);
  app.on(ACTION_KEYPAD_BACKSPACE, &EpubReaderPercentSelectionActivity::onKeypadBackspaceEvent, this);
  app.setScreen(&EpubReaderPercentSelectionActivity::percentScreen, this);
  // Touch devices always use the keypad; it has no slider fallback to return to.
  keypadActive = mappedInput.hasTouch();
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

int EpubReaderPercentSelectionActivity::deltaForDisplayStep(const int displaySteps) const {
  // StablePage steps by whole pages; Percent stores centipercent, so a "1" or "10"
  // step shown to the user is 100x that in the stored unit.
  return mode == Mode::StablePage ? displaySteps : displaySteps * 100;
}

void EpubReaderPercentSelectionActivity::adjustValue(const int delta) {
  if (mode == Mode::StablePage) {
    value = adjustStablePage(value, delta, maximum);
    requestUpdate();
    return;
  }
  // Wrap using a 10000-value ring (0.00% and 100.00% are the same wrap point), but keep
  // 10000 as the natural landing value when reached without crossing the boundary.
  const int raw = static_cast<int>(value) + delta;
  if (raw > 0 && raw % 10000 == 0) {
    value = 10000;
  } else {
    value = static_cast<uint32_t>(((raw % 10000) + 10000) % 10000);
  }
  requestUpdate();
}

bool EpubReaderPercentSelectionActivity::isKeypadVisible() const { return mappedInput.hasTouch() || keypadActive; }

void EpubReaderPercentSelectionActivity::enterKeypad() {
  keypadActive = true;
  seedKeypadEntryFromValue();
  keypadRow = 0;
  keypadCol = 0;
  keypadBackspaceFocused = false;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::exitKeypad() {
  keypadActive = false;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::seedKeypadEntryFromValue() {
  if (mode == Mode::StablePage) {
    std::snprintf(entryText, sizeof(entryText), "%lu", static_cast<unsigned long>(value));
  } else if (value % 100 == 0) {
    std::snprintf(entryText, sizeof(entryText), "%lu", static_cast<unsigned long>(value / 100));
  } else if (value % 10 == 0) {
    std::snprintf(entryText, sizeof(entryText), "%lu.%lu", static_cast<unsigned long>(value / 100),
                  static_cast<unsigned long>((value % 100) / 10));
  } else {
    std::snprintf(entryText, sizeof(entryText), "%lu.%02lu", static_cast<unsigned long>(value / 100),
                  static_cast<unsigned long>(value % 100));
  }
  entryLen = static_cast<uint8_t>(std::strlen(entryText));
}

void EpubReaderPercentSelectionActivity::moveKeypadFocus(const int rowDelta, const int colDelta) {
  if (keypadBackspaceFocused) {
    if (rowDelta > 0 || colDelta != 0) keypadBackspaceFocused = false;
    requestUpdate();
    return;
  }
  if (rowDelta < 0 && keypadRow == 0) {
    keypadBackspaceFocused = true;
    requestUpdate();
    return;
  }
  keypadRow = ((keypadRow + rowDelta) % 4 + 4) % 4;
  keypadCol = ((keypadCol + colDelta) % 3 + 3) % 3;
  // The '.' cell (row 3, col 1) is disabled outside Percent mode; step past it in
  // whichever direction focus was already moving rather than let it land there.
  if (mode != Mode::Percent && keypadRow == 3 && keypadCol == 1) {
    if (colDelta != 0) {
      keypadCol = ((keypadCol + colDelta) % 3 + 3) % 3;
    } else if (rowDelta != 0) {
      keypadRow = ((keypadRow + rowDelta) % 4 + 4) % 4;
    } else {
      keypadCol = 0;
    }
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::activateKeypadFocus() {
  if (keypadBackspaceFocused) {
    backspaceEntry();
    return;
  }
  // Grid layout: 1 2 3 / 4 5 6 / 7 8 9 / 0 . OK (matches handleKeypadValue's key values).
  static constexpr int16_t kGridValues[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, kKeypadDot, kKeypadOk};
  const int idx = keypadRow * 3 + keypadCol;
  if (idx == 10 && mode != Mode::Percent) return;  // '.' is disabled outside Percent mode
  handleKeypadValue(kGridValues[idx]);
}

void EpubReaderPercentSelectionActivity::handleKeypadValue(const int16_t keyValue) {
  if (keyValue == kKeypadOk) {
    // Nothing typed: do nothing rather than silently confirm whatever `value` already
    // held while the readout reads "0" (which is what an empty buffer displays as).
    if (entryLen == 0) return;
    confirmKeypad();
  } else if (keyValue == kKeypadDot) {
    appendDecimalPoint();
  } else {
    appendDigit(static_cast<char>('0' + keyValue));
  }
}

void EpubReaderPercentSelectionActivity::appendDigit(const char digit) {
  if (entryLen >= sizeof(entryText) - 1) return;
  // Reject a digit that would push the typed value out of range, rather than let the
  // readout show a number OK would silently clamp to something else.
  char candidate[sizeof(entryText)];
  std::memcpy(candidate, entryText, entryLen);
  candidate[entryLen] = digit;
  candidate[entryLen + 1] = 0;
  if (mode == Mode::Percent) {
    const char* dot = static_cast<const char*>(std::memchr(entryText, '.', entryLen));
    if (dot != nullptr && (entryText + entryLen) - (dot + 1) >= 2) return;
    if (std::strtof(candidate, nullptr) > 100.0f) return;
  } else {
    const unsigned long candidateValue = std::strtoul(candidate, nullptr, 10);
    if (candidateValue == 0 || candidateValue > maximum) return;
  }
  entryText[entryLen++] = digit;
  entryText[entryLen] = 0;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::appendDecimalPoint() {
  if (mode != Mode::Percent) return;
  // Require a leading digit ("0." rather than bare "."): otherwise the readout
  // would show "." while OK silently confirms 0%.
  if (entryLen == 0) return;
  for (uint8_t i = 0; i < entryLen; ++i) {
    if (entryText[i] == '.') return;  // one decimal point at most
  }
  if (entryLen >= sizeof(entryText) - 1) return;
  entryText[entryLen++] = '.';
  entryText[entryLen] = 0;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::backspaceEntry() {
  if (entryLen == 0) return;
  entryText[--entryLen] = 0;
  if (entryLen == 0) keypadBackspaceFocused = false;
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::confirmKeypad() {
  app.clearTapFlash();
  if (entryLen > 0) {
    if (mode == Mode::StablePage) {
      value = clampStablePage(static_cast<uint32_t>(std::strtoul(entryText, nullptr, 10)), maximum);
    } else {
      const float parsed = std::strtof(entryText, nullptr);
      value = static_cast<uint32_t>(std::lround(std::clamp(parsed, 0.0f, 100.0f) * 100.0f));
    }
  }
  confirm();
}

void EpubReaderPercentSelectionActivity::onKeypadKeyEvent(const fui::ActionEvent& event, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->handleKeypadValue(static_cast<int16_t>(event.value));
}

void EpubReaderPercentSelectionActivity::onKeypadBackspaceEvent(const fui::ActionEvent&, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->backspaceEntry();
}

void EpubReaderPercentSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderPercentSelectionActivity::confirm() {
  if (mode == Mode::StablePage) {
    setResult(PageResult{value});
  } else {
    setResult(PercentResult{static_cast<float>(value) / 100.0f});
  }
  finish();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Touch goes through the FreeInkApp: render() registered the keypad grid and its
  // backspace icon. Runs before the Back handler for the same reason as elsewhere in
  // this codebase: a release routed to a UI control must not also read as a gesture.
  fui::InputSnapshot snap{};
  if (uiReady) {
    snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  // A long-press Confirm already fired this press: swallow input until it's physically
  // released so the next press starts cleanly.
  if (confirmLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      confirmLongPressFired = false;
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, screen);
  if (TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (keypadActive && !mappedInput.hasTouch()) {
      // Back out of keypad entry to the slider first; a second Back cancels the screen.
      exitKeypad();
    } else {
      cancel();
    }
    return;
  }

  // getHeldTime() times the current chord from whichever button went down first, not
  // Confirm specifically. Left/Right/Up/Down auto-repeat while held on this screen, so
  // holding one of them for a second and then pressing Confirm would otherwise read as
  // an instant long-press. Only treat Confirm's own hold as a long-press.
  const bool anyDirectionHeld = mappedInput.isPressed(MappedInputManager::Button::Left) ||
                                mappedInput.isPressed(MappedInputManager::Button::Right) ||
                                mappedInput.isPressed(MappedInputManager::Button::Up) ||
                                mappedInput.isPressed(MappedInputManager::Button::Down);

  if (isKeypadVisible()) {
    if (!anyDirectionHeld && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= kLongPressMs) {
      confirmLongPressFired = true;
      backspaceEntry();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateKeypadFocus();
      return;
    }
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveKeypadFocus(0, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveKeypadFocus(0, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveKeypadFocus(-1, 0); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveKeypadFocus(1, 0); });
    return;
  }

  // Slider mode (non-touch only; touch is always keypadVisible above).
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    adjustValue(deltaForDisplayStep(kLargeStep));
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left) {
    adjustValue(deltaForDisplayStep(-kLargeStep));
    return;
  }

  if (!anyDirectionHeld && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= kLongPressMs) {
    confirmLongPressFired = true;
    enterKeypad();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustValue(deltaForDisplayStep(-kSmallStep)); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustValue(deltaForDisplayStep(kSmallStep)); });

  buttonNavigator.onPressAndContinuous({mappedInput.sliderIncreaseButton()},
                                       [this] { adjustValue(deltaForDisplayStep(kLargeStep)); });
  buttonNavigator.onPressAndContinuous({mappedInput.sliderDecreaseButton()},
                                       [this] { adjustValue(deltaForDisplayStep(-kLargeStep)); });
}

void EpubReaderPercentSelectionActivity::percentScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderPercentSelectionActivity*>(user)->buildPercentScreen(screen);
}

void EpubReaderPercentSelectionActivity::buildPercentScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Start below CrossInk's shared back header; its touch-device height differs from
  // the legacy theme header height.
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y) +
                                                      metrics.verticalSpacing * 4),
                                 static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                 static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                 static_cast<int16_t>(safe.x)});

  char line[64];

  if (isKeypadVisible()) {
    buildKeypadScreen(screen, line, sizeof(line));
    return;
  }

  // Percent/page readout, centered above the slider.
  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutLh = screen.target().lineHeight(readout.font);
  if (mode == Mode::StablePage) {
    snprintf(line, sizeof(line), "%lu / %lu", static_cast<unsigned long>(value), static_cast<unsigned long>(maximum));
  } else {
    snprintf(line, sizeof(line), "%lu.%02lu%%", static_cast<unsigned long>(value / 100),
             static_cast<unsigned long>(value % 100));
  }
  screen.target().text(screen.takeTop(readoutLh, theme.spaceLg), line, readout);

  // The slider is a visual-only indicator here: it only renders when the device has no
  // touch hardware (isKeypadVisible() already handled the touch case above), so it can
  // never be dragged. Physical buttons drive `value` directly via adjustValue().
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  const fui::Rect row = screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset);

  fui::SliderProps sliderProps;
  sliderProps.value =
      mode == Mode::StablePage ? stablePageToPermille(value, maximum) : static_cast<int32_t>(value / 10);
  sliderProps.max = 1000;
  fui::slider(screen.frame(), row, sliderProps);

  // Two-line step hint built from separate label + value strings (front buttons = fine step, side
  // buttons = coarse step), so the layout doesn't depend on a separator hidden in translated text.
  fui::TextStyle hint = ReaderSliderHints::style(theme);
  const int16_t hintLh = screen.target().lineHeight(hint.font);
  snprintf(line, sizeof(line), mode == Mode::StablePage ? "%s: %d" : "%s: %d%%", I18N.get(StrId::STR_FRONT_BUTTONS),
           kSmallStep);
  screen.target().text(screen.takeTop(hintLh, theme.spaceSm), line, hint);
  snprintf(line, sizeof(line), mode == Mode::StablePage ? "%s: %d" : "%s: %d%%", I18N.get(StrId::STR_SIDE_BUTTONS),
           kLargeStep);
  screen.target().text(screen.takeTop(hintLh, theme.spaceSm), line, hint);

  // Discoverability for the keypad escape hatch: names the actual on-screen label for
  // Confirm on this screen (STR_SELECT), so the hint always matches what's shown below.
  snprintf(line, sizeof(line), I18N.get(StrId::STR_HOLD_FOR_KEYBOARD), tr(STR_SELECT));
  hint.font = theme.smallText.font;
  screen.target().text(screen.takeTop(screen.target().lineHeight(hint.font)), line, hint);
}

void EpubReaderPercentSelectionActivity::buildKeypadScreen(UiApp::ScreenType& screen, char* line, size_t lineSize) {
  const auto& theme = screen.theme();

  // Readout shows the raw typed digits (or a neutral placeholder), not a reformatted
  // number, so a trailing "." typed by the user stays visible while editing.
  const char* typed = entryLen > 0 ? entryText : "0";
  if (mode == Mode::StablePage) {
    snprintf(line, lineSize, "%s / %lu", typed, static_cast<unsigned long>(maximum));
  } else {
    snprintf(line, lineSize, "%s%%", typed);
  }

  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutLh = screen.target().lineHeight(readout.font);
  const int16_t rowH = std::max<int16_t>(theme.rowHeight, static_cast<int16_t>(readoutLh + 16));
  const bool beside = mappedInput.hasTouch() && renderer.getScreenWidth() > renderer.getScreenHeight();
  const auto layout = fui::keyGridReadoutLayout(
      screen.contentRect(), rowH, static_cast<int16_t>(screen.target().measureText(readout.font, line, readout).width),
      theme.spaceLg, theme.spaceLg, beside);
  const fui::Rect readoutRow = layout.readout;
  const fui::Rect iconRect = layout.backspace;

  if (mappedInput.hasTouch()) {
    // Backspace stays outside the familiar 1-9/0/./OK grid.
    screen.target().text(readoutRow, line, readout);

    fui::ButtonProps backspaceBtn;
    backspaceBtn.icon = fui::bitmapFromIcon(icon_backspace_28);
    backspaceBtn.action = ACTION_KEYPAD_BACKSPACE;
    backspaceBtn.inputMask = fui::InputTouch;
    backspaceBtn.enabled = entryLen > 0;
    screen.button(backspaceBtn, iconRect);
  } else {
    screen.target().text(readoutRow, line, readout);

    // Button-only devices move Up from the first keypad row to this control;
    // Confirm deletes the final digit. The FreeInk button supplies the same icon
    // and selected treatment as touch, while the activity owns directional focus.
    fui::ButtonProps backspaceBtn;
    backspaceBtn.icon = fui::bitmapFromIcon(icon_backspace_28);
    backspaceBtn.action = ACTION_KEYPAD_BACKSPACE;
    backspaceBtn.inputMask = fui::InputNone;
    backspaceBtn.state = keypadBackspaceFocused ? fui::StateSelected : fui::StateNormal;
    backspaceBtn.enabled = entryLen > 0;
    screen.button(backspaceBtn, iconRect);
  }

  const fui::Rect gridArea = layout.grid;

  static const char* const kDigitLabels[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  fui::KeyGridKey keys[12];
  for (int i = 0; i < 9; ++i) {
    keys[i].label = kDigitLabels[i + 1];
    keys[i].value = static_cast<int16_t>(i + 1);
  }
  keys[9].label = kDigitLabels[0];
  keys[9].value = 0;
  keys[10].label = ".";
  keys[10].value = kKeypadDot;
  keys[10].enabled = mode == Mode::Percent;
  keys[11].label = tr(STR_OK);
  keys[11].value = kKeypadOk;

  fui::KeyGridProps gridProps;
  gridProps.keys = keys;
  gridProps.rows = 4;
  gridProps.cols = 3;
  gridProps.action = ACTION_KEYPAD_KEY;
  gridProps.inputMask = fui::InputTouch;
  gridProps.labelText = theme.bodyText;
  gridProps.labelText.align = fui::TextAlign::Center;
  gridProps.gap = theme.spaceSm;
  const int16_t cellH = static_cast<int16_t>((gridArea.height - gridProps.gap * 3) / 4);
  const int16_t cellW = static_cast<int16_t>((gridArea.width - gridProps.gap * 2) / 3);
  gridProps.minTouchSize = std::min<int16_t>(theme.minTouchSize, std::max<int16_t>(1, std::min(cellH, cellW)));
  gridProps.radius = 3;
  // Non-touch shows which key directional nav is on; touch has no such focus concept.
  gridProps.selectedIndex =
      mappedInput.hasTouch() || keypadBackspaceFocused ? int16_t{-1} : static_cast<int16_t>(keypadRow * 3 + keypadCol);
  fui::keyGrid(screen.frame(), gridArea, gridProps);
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, screen);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header,
                                mode == Mode::StablePage ? tr(STR_GO_TO_STABLE_PAGE) : tr(STR_GO_TO_PERCENT), true);
  } else {
    GUI.drawHeader(renderer, header, mode == Mode::StablePage ? tr(STR_GO_TO_STABLE_PAGE) : tr(STR_GO_TO_PERCENT),
                   nullptr, true);
  }

  // Percent/page readout, keypad or slider, and step controls render through the app.
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  // Button hints follow the current front button layout and auto-hide on touch devices.
  const bool percentSlider = mode == Mode::Percent && !isKeypadVisible();
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                            percentSlider ? "-1" : tr(STR_DIR_LEFT), percentSlider ? "+1" : tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
