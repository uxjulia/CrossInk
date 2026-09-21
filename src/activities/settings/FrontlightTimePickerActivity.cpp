#include "FrontlightTimePickerActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/icons/keyboardIcons.h"
#include "fontIds.h"
#include "util/FrontlightSchedule.h"

namespace {
namespace fui = freeink::ui;

constexpr int kFieldHeight = 60;
constexpr int kHourWidth = 76;
constexpr int kMinuteWidth = 88;
constexpr int kPeriodWidth = 96;
constexpr int kFieldGap = 14;
constexpr int kColonGap = 8;
constexpr int kKeyboardRows = 4;
constexpr fui::ActionId kKeyboardAction = 1;
constexpr int16_t kKeyboardBackspace = -1;
constexpr int16_t kKeyboardOk = -2;

struct PickerLayout {
  Rect hourRect;
  Rect minuteRect;
  Rect periodRect;
  Rect backspaceRect;
  int colonX;
  int textY;
};

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

fui::Rect keyboardRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = metrics.keyboardKeySpacing;
  const int height = kKeyboardRows * metrics.keyboardKeyHeight + (kKeyboardRows - 1) * gap;
  const int width = renderer.getScreenWidth() * metrics.keyboardWidthPercent / 100;
  const int x = (renderer.getScreenWidth() - width) / 2;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - height +
                metrics.keyboardVerticalOffset;
  return {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width), static_cast<int16_t>(height)};
}

PickerLayout getPickerLayout(const GfxRenderer& renderer, const MappedInputManager& mappedInput) {
  const int colonWidth = renderer.getTextWidth(UI_12_FONT_ID, ":", EpdFontFamily::BOLD);
  const int backspaceWidth = mappedInput.hasTouch() ? kFieldGap + kFieldHeight : 0;
  const int fieldsWidth = kHourWidth + kFieldGap + kColonGap + colonWidth + kColonGap + kMinuteWidth + kFieldGap +
                          kPeriodWidth + backspaceWidth;
  const int startX = (renderer.getScreenWidth() - fieldsWidth) / 2;
  int fieldY = renderer.getScreenHeight() / 2 - kFieldHeight / 2;
  if (mappedInput.hasTouch()) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const int contentTop = header.y + header.height;
    const int contentHeight = keyboardRect(renderer).y - contentTop;
    fieldY = contentTop + (contentHeight - kFieldHeight) / 2;
  }

  int x = startX;
  const Rect hourRect{x, fieldY, kHourWidth, kFieldHeight};
  x += kHourWidth + kFieldGap;
  const int colonX = x;
  x += kColonGap + colonWidth + kColonGap;
  const Rect minuteRect{x, fieldY, kMinuteWidth, kFieldHeight};
  x += kMinuteWidth + kFieldGap;
  const Rect periodRect{x, fieldY, kPeriodWidth, kFieldHeight};
  x += kPeriodWidth + kFieldGap;
  const Rect backspaceRect{x, fieldY, kFieldHeight, kFieldHeight};
  return {hourRect,      minuteRect, periodRect,
          backspaceRect, colonX,     fieldY + (kFieldHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2};
}
}  // namespace

FrontlightTimePickerActivity::FrontlightTimePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const StrId titleId, const uint16_t initialTimeOfDay)
    : Activity("FrontlightTimePicker", renderer, mappedInput), titleId(titleId), initialTimeOfDay(initialTimeOfDay) {}

void FrontlightTimePickerActivity::onEnter() {
  Activity::onEnter();
  const FrontlightSchedule::TimeOfDay time = FrontlightSchedule::timeOfDayFromMinutes(initialTimeOfDay);
  hour12 = time.hour12;
  minute = time.minute;
  isPm = time.isPm;
  activeField = Field::Hour;
  clearNumericEntry();
  keyboardTouchRouter.overrideValue = kKeyboardBackspace;
  keyboardTouchRouter.reset();
  keyboardInteractionsReady.store(false, std::memory_order_release);
  requestUpdate();
}

void FrontlightTimePickerActivity::adjustActiveField(const int delta) {
  clearNumericEntry();
  switch (activeField) {
    case Field::Hour:
      hour12 = static_cast<uint8_t>((static_cast<int>(hour12) - 1 + delta + 12) % 12 + 1);
      break;
    case Field::Minute:
      minute = static_cast<uint8_t>((static_cast<int>(minute) + delta + 60) % 60);
      break;
    case Field::Period:
      isPm = !isPm;
      break;
    case Field::Count:
      break;
  }
}

void FrontlightTimePickerActivity::selectNextField(const int delta) {
  constexpr int fieldCount = static_cast<int>(Field::Count);
  activeField = static_cast<Field>((static_cast<int>(activeField) + delta + fieldCount) % fieldCount);
  clearNumericEntry();
}

void FrontlightTimePickerActivity::complete() {
  setResult(IntervalResult{FrontlightSchedule::minutesFromTimeOfDay(hour12, minute, isPm)});
  finish();
}

bool FrontlightTimePickerActivity::selectFieldAt(const int x, const int y, const bool togglePeriod) {
  const PickerLayout layout = getPickerLayout(renderer, mappedInput);
  if (contains(layout.hourRect, x, y)) {
    activeField = Field::Hour;
  } else if (contains(layout.minuteRect, x, y)) {
    activeField = Field::Minute;
  } else if (contains(layout.periodRect, x, y)) {
    activeField = Field::Period;
    if (togglePeriod) isPm = !isPm;
  } else {
    return false;
  }
  clearNumericEntry();
  requestUpdate();
  return true;
}

void FrontlightTimePickerActivity::clearNumericEntry() {
  numericEntry = 0;
  numericEntryDigits = 0;
}

void FrontlightTimePickerActivity::enterDigit(const uint8_t digit) {
  if (activeField == Field::Period) return;

  if (activeField == Field::Hour) {
    if (numericEntryDigits == 0) {
      if (digit == 0) {
        hour12 = 12;
      } else {
        hour12 = digit;
        numericEntry = digit;
        numericEntryDigits = digit == 1 ? 1 : 0;
      }
    } else {
      const uint8_t candidate = static_cast<uint8_t>(numericEntry * 10 + digit);
      if (candidate >= 10 && candidate <= 12) {
        hour12 = candidate;
      }
      clearNumericEntry();
    }
  } else if (numericEntryDigits == 0) {
    numericEntry = digit;
    numericEntryDigits = 1;
  } else {
    const uint8_t candidate = static_cast<uint8_t>(numericEntry * 10 + digit);
    if (candidate < 60) {
      minute = candidate;
    }
    clearNumericEntry();
  }
  requestUpdate();
}

void FrontlightTimePickerActivity::handleKeyboardValue(const int16_t value) {
  if (value >= 0 && value <= 9) {
    enterDigit(static_cast<uint8_t>(value));
  } else if (value == kKeyboardBackspace) {
    clearNumericEntry();
    requestUpdate();
  } else if (value == kKeyboardOk) {
    complete();
  }
}

void FrontlightTimePickerActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    complete();
    return;
  }

  if (mappedInput.hasTouch()) {
    int touchDownX = 0;
    int touchDownY = 0;
    if (mappedInput.wasScreenTouchDown(touchDownX, touchDownY)) {
      if (selectFieldAt(touchDownX, touchDownY, false)) return;
    }
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    if (tapped) {
      if (selectFieldAt(tapX, tapY, true)) return;
    }

    if (keyboardInteractionsReady.load(std::memory_order_acquire)) {
      unsigned long heldMs = 0;
      int candidateX = 0;
      int candidateY = 0;
      const bool tapCandidate = mappedInput.isScreenTouchTapCandidate(candidateX, candidateY, heldMs);
      int heldX = 0;
      int heldY = 0;
      const bool inContact = mappedInput.isScreenTouchHeld(heldX, heldY);
      const fui::TouchHoldRouter::Result result = keyboardTouchRouter.update(
          keyboardInteractions, tapCandidate, static_cast<int16_t>(candidateX), static_cast<int16_t>(candidateY),
          tapped, static_cast<int16_t>(tapX), static_cast<int16_t>(tapY), inContact, millis());
      if (result.event) {
        handleKeyboardValue(result.event.value);
        return;
      }
      if (result.activeChanged) requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectNextField(-1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectNextField(+1);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void FrontlightTimePickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, I18N.get(titleId), false);
  } else {
    GUI.drawHeader(renderer, header, I18N.get(titleId));
  }

  const PickerLayout layout = getPickerLayout(renderer, mappedInput);
  char hourText[4];
  char minuteText[4];
  snprintf(hourText, sizeof(hourText), "%u", static_cast<unsigned>(hour12));
  if (activeField == Field::Minute && numericEntryDigits == 1) {
    snprintf(minuteText, sizeof(minuteText), "%u_", static_cast<unsigned>(numericEntry));
  } else {
    snprintf(minuteText, sizeof(minuteText), "%02u", static_cast<unsigned>(minute));
  }
  const char* periodText = I18N.get(isPm ? StrId::STR_PM : StrId::STR_AM);

  auto drawField = [&](const char* text, const Rect& rect, const Field field) {
    const bool selected = field == activeField;
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, selected ? Color::LightGray : Color::White);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    if (selected) renderer.drawRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, true);
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, layout.textY, text, true,
                      EpdFontFamily::BOLD);
  };

  drawField(hourText, layout.hourRect, Field::Hour);
  renderer.drawText(UI_12_FONT_ID, layout.colonX, layout.textY, ":", true, EpdFontFamily::BOLD);
  drawField(minuteText, layout.minuteRect, Field::Minute);
  drawField(periodText, layout.periodRect, Field::Period);

  if (mappedInput.hasTouch()) {
    fui::GfxRendererTarget target(renderer);
    target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
    target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
    const fui::DeviceContext device = target.deviceContext();
    const fui::InputSnapshot noInput{};
    keyboardInteractions.beginPublishCycle();
    fui::Frame<48> frame(target, device, noInput, keyboardInteractions);
    fui::ButtonProps backspace;
    backspace.icon = fui::bitmapFromIcon(icon_backspace_28);
    backspace.action = kKeyboardAction;
    backspace.value = kKeyboardBackspace;
    backspace.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
    backspace.enabled = numericEntryDigits > 0;
    fui::button(frame,
                {static_cast<int16_t>(layout.backspaceRect.x), static_cast<int16_t>(layout.backspaceRect.y),
                 static_cast<int16_t>(layout.backspaceRect.width), static_cast<int16_t>(layout.backspaceRect.height)},
                backspace);

    static constexpr const char* kDigitLabels[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    fui::KeyGridKey keys[12];
    for (int i = 0; i < 9; ++i) {
      keys[i].label = kDigitLabels[i + 1];
      keys[i].value = static_cast<int16_t>(i + 1);
    }
    keys[9].label = kDigitLabels[0];
    keys[9].value = 0;
    keys[10].kind = fui::KeyKind::Disabled;
    keys[11].label = tr(STR_OK);
    keys[11].kind = fui::KeyKind::Ok;
    keys[11].value = kKeyboardOk;

    fui::KeyGridProps props;
    props.keys = keys;
    props.rows = kKeyboardRows;
    props.cols = 3;
    props.action = kKeyboardAction;
    props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
    props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
    const auto& metrics = UITheme::getInstance().getMetrics();
    props.gap = static_cast<int16_t>(metrics.keyboardKeySpacing);
    const fui::Rect kbRect = keyboardRect(renderer);
    fui::keyGrid(frame, kbRect, props);
    keyboardInteractions.publish();
    keyboardInteractionsReady.store(true, std::memory_order_release);
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
