#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "DeviceCapabilities.h"
#include "components/SliderValue.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "util/InputReleaseGuard.h"
#if CROSSINK_APP_CAP_TOUCH
#include "components/UiAppHelpers.h"
#endif
#include "fontIds.h"

#if CROSSINK_APP_CAP_TOUCH
namespace fui = freeink::ui;
#endif

namespace {
constexpr int TOUCH_STEP_BUTTON_SIZE = 56;
constexpr int TOUCH_STEP_BUTTON_GAP = 32;
constexpr int TOUCH_STEP_LABEL_HEIGHT = 56;
#if CROSSINK_APP_CAP_TOUCH
constexpr int16_t READER_SLIDER_CONTROL_HEIGHT = 30;
constexpr int16_t READER_SLIDER_SCALE_GAP = 4;
#endif

Rect touchStepButtonRect(const Rect& screen, const int index) {
  const int totalWidth = TOUCH_STEP_BUTTON_SIZE * 4 + TOUCH_STEP_BUTTON_GAP * 3;
  const int x = screen.x + (screen.width - totalWidth) / 2 + index * (TOUCH_STEP_BUTTON_SIZE + TOUCH_STEP_BUTTON_GAP);
  return Rect{x, 220, TOUCH_STEP_BUTTON_SIZE, TOUCH_STEP_BUTTON_SIZE};
}

Rect touchStepLabelRect(const Rect& screen, const int index) {
  constexpr int top = 176;
  const int width = screen.width / 4;
  const int x = screen.x + index * width;
  const int right = index == 3 ? screen.x + screen.width : x + width;
  return Rect{x, top, right - x, TOUCH_STEP_LABEL_HEIGHT};
}

TouchActionButtons::Layout touchActionLayout(const Rect& screen) {
  constexpr int sideMargin = 24;
  constexpr int bottomMargin = 12;
  constexpr int totalHeight = TouchActionButtons::kDefaultHeight * 2 + TouchActionButtons::kDefaultGap;
  return TouchActionButtons::vertical(Rect{screen.x + sideMargin, screen.y + screen.height - bottomMargin - totalHeight,
                                           std::max(1, screen.width - sideMargin * 2), totalHeight},
                                      2);
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

void formatCompactSeconds(const int seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%ds", seconds);
  } else if (seconds % 60 == 0) {
    snprintf(buf, len, "%dm", seconds / 60);
  } else {
    snprintf(buf, len, "%dm %ds", seconds / 60, seconds % 60);
  }
}
}  // namespace

IntervalSelectionActivity::IntervalSelectionActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName, const StrId titleId,
    const int initialValue, const int minValue, const int maxValue, const int smallStep, const int largeStep,
    const StrId valueFormatId, const bool readerActivity, const bool allowPowerAsConfirm,
    const bool ignoreInitialConfirmRelease, const bool showPercentValue, const StrId maxBoundaryLabelId,
    const bool overrideDisabledReaderTouchscreen, const bool showTouchHeaderBackButton, ValueFormatter valueFormatter,
    const int tapStep, const bool useReaderSlider)
    : Activity(activityName, renderer, mappedInput),
      titleId(titleId),
      valueFormatId(valueFormatId),
      maxBoundaryLabelId(maxBoundaryLabelId),
      value(initialValue),
      minValue(minValue),
      maxValue(maxValue),
      smallStep(smallStep),
      largeStep(largeStep),
      readerActivity(readerActivity),
      allowPowerAsConfirm(allowPowerAsConfirm),
      ignoreConfirmRelease(ignoreInitialConfirmRelease),
      showPercentValue(showPercentValue),
      overrideDisabledReaderTouchscreen(overrideDisabledReaderTouchscreen),
      showTouchHeaderBackButton(showTouchHeaderBackButton),
      valueFormatter(valueFormatter),
      tapStep(tapStep),
#if CROSSINK_APP_CAP_TOUCH
      useReaderSlider(useReaderSlider),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext())
#else
      useReaderSlider(useReaderSlider)
#endif
{
}

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

int IntervalSelectionActivity::tappedValue(const int candidate) const {
  if (tapStep <= 1) return clampedValue(candidate);
  return snapSliderTapValue(candidate, minValue, maxValue, tapStep);
}

bool IntervalSelectionActivity::usesReaderSlider() const {
#if CROSSINK_APP_CAP_TOUCH
  return useReaderSlider && mappedInput.hasTouchHardware();
#else
  return false;
#endif
}

void IntervalSelectionActivity::formatValue(char* const buf, const size_t len) const {
  if (valueFormatter != nullptr) {
    valueFormatter(value, buf, len);
  } else if (maxBoundaryLabelId != StrId::STR_NONE_OPT && value == maxValue) {
    snprintf(buf, len, "%s", I18N.get(maxBoundaryLabelId));
  } else if (showPercentValue) {
    snprintf(buf, len, "%d%%", value);
  } else if (valueFormatId == StrId::STR_SECONDS_VALUE_FORMAT) {
    formatCompactSeconds(value, buf, len);
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(buf, len, I18N.get(valueFormatId), static_cast<unsigned int>(value));
  } else {
    snprintf(buf, len, "%d", value);
  }
}

#if CROSSINK_APP_CAP_TOUCH
void IntervalSelectionActivity::formatEndpoint(const int endpoint, char* const buf, const size_t len) const {
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && endpoint == maxValue) {
    snprintf(buf, len, "%s", I18N.get(maxBoundaryLabelId));
  } else if (showPercentValue) {
    snprintf(buf, len, "%d%%", endpoint);
  } else {
    snprintf(buf, len, "%d", endpoint);
  }
}
#endif

bool IntervalSelectionActivity::usesTextTouchStepControls() const {
  return titleId == StrId::STR_TIME_TO_SLEEP || titleId == StrId::STR_AUTO_TURN_INTERVAL_SECONDS ||
         titleId == StrId::STR_LINE_SPACING || titleId == StrId::STR_TOP_BOTTOM || titleId == StrId::STR_LEFT_RIGHT;
}

#if CROSSINK_APP_CAP_TOUCH
void IntervalSelectionActivity::sliderScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<IntervalSelectionActivity*>(user)->buildSliderScreen(screen);
}

void IntervalSelectionActivity::buildSliderScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect safe = screen.frame().safeRect();
  const Rect touchScreen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const auto actions = touchActionLayout(touchScreen);
  const int contentTop = header.y + header.height + metrics.verticalSpacing;
  const int contentBottom = actions.buttons[0].y - metrics.verticalSpacing;
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(std::max(0, contentTop - safe.y)), 0,
                                      static_cast<int16_t>(std::max(0, safe.bottom() - contentBottom)), 0});

  char valueLabel[32] = {};
  char minimumLabel[16] = {};
  char maximumLabel[16] = {};
  formatValue(valueLabel, sizeof(valueLabel));
  formatEndpoint(minValue, minimumLabel, sizeof(minimumLabel));
  formatEndpoint(maxValue, maximumLabel, sizeof(maximumLabel));

  fui::TextStyle valueStyle = screen.theme().bodyText;
  valueStyle.bold = true;
  const int16_t lineHeight = screen.target().lineHeight(valueStyle.font);
  const int16_t rowHeight = static_cast<int16_t>(lineHeight + READER_SLIDER_SCALE_GAP + READER_SLIDER_CONTROL_HEIGHT +
                                                 lineHeight + READER_SLIDER_SCALE_GAP);
  const int16_t controlTopInset = static_cast<int16_t>(lineHeight + READER_SLIDER_SCALE_GAP);
  const int16_t top = std::max<int16_t>(
      0, static_cast<int16_t>((screen.body().height - READER_SLIDER_CONTROL_HEIGHT) / 2 - controlTopInset));
  screen.spacer(top);
  const fui::Rect row = screen.takeTop(rowHeight);
  const fui::Rect band{row.x, static_cast<int16_t>(row.y + controlTopInset), row.width, READER_SLIDER_CONTROL_HEIGHT};
  const int16_t stepWidth = std::max<int16_t>(band.height, screen.theme().rowHeight);
  const int16_t sideGap = static_cast<int16_t>(stepWidth + screen.theme().spaceSm);

  fui::ButtonProps step;
  step.text = screen.theme().bodyText;
  step.text.bold = true;
  step.styles = fui::plainStyles();
  step.inputMask = fui::InputTouch;
  step.minTouchSize = stepWidth;
  step.label = "-";
  step.action = ACTION_STEP;
  step.value = -1;
  step.hitPadding.right = screen.theme().spaceSm;
  fui::button(screen.frame(), fui::Rect{band.x, band.y, stepWidth, band.height}, step);

  const int16_t plusX = static_cast<int16_t>(band.right() - stepWidth);
  step.label = "+";
  step.value = 1;
  step.hitPadding.left = screen.theme().spaceSm;
  step.hitPadding.right = 0;
  fui::button(screen.frame(), fui::Rect{plusX, band.y, stepWidth, band.height}, step);

  const fui::Rect track = band.inset(fui::Insets{0, sideGap, 0, sideGap});
  const int range = std::max(1, maxValue - minValue);
  fui::SliderProps slider;
  slider.value = static_cast<int32_t>((value - minValue) * 1000 / range);
  slider.max = 1000;
  slider.action = ACTION_SLIDER;
  slider.inputMask = fui::InputTouch | fui::InputDrag;
  slider.trackHeight = 3;
  slider.knobWidth = 10;
  slider.knobHeight = 22;
  slider.horizontalPadding = 0;
  slider.minTouchSize = screen.theme().minTouchSize;
  slider.radius = 0;
  slider.border = fui::Paint::none();

  const int16_t knobWidth = std::max<int16_t>(4, slider.knobWidth);
  const int16_t travel = std::max<int16_t>(0, static_cast<int16_t>(track.width - knobWidth));
  const int16_t knobCenter =
      static_cast<int16_t>(track.x + knobWidth / 2 + static_cast<int32_t>(travel) * slider.value / slider.max);
  const fui::Size valueSize = screen.target().measureText(valueStyle.font, valueLabel, valueStyle);
  const fui::Rect valueLane{track.x, row.y, track.width, lineHeight};
  fui::Rect valueRect{static_cast<int16_t>(knobCenter - valueSize.width / 2), valueLane.y, valueSize.width,
                      valueLane.height};
  valueRect.x = std::max<int16_t>(valueLane.x, std::min<int16_t>(valueRect.x, valueLane.right() - valueRect.width));
  valueStyle.align = fui::TextAlign::Left;
  screen.target().text(valueRect, valueLabel, valueStyle);
  fui::slider(screen.frame(), track, slider);

  fui::TextStyle endpointStyle = screen.theme().bodyText;
  endpointStyle.align = fui::TextAlign::Center;
  const int16_t endpointY = static_cast<int16_t>(band.bottom() + READER_SLIDER_SCALE_GAP);
  screen.target().text(fui::Rect{band.x, endpointY, stepWidth, lineHeight}, minimumLabel, endpointStyle);
  screen.target().text(fui::Rect{plusX, endpointY, stepWidth, lineHeight}, maximumLabel, endpointStyle);
}

void IntervalSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  if (event.dragPermille < 0) return;
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  const int range = std::max(1, self->maxValue - self->minValue);
  const int candidate = self->minValue + (static_cast<int>(event.dragPermille) * range + 500) / 1000;
  self->value = self->sliderTapPending ? self->tappedValue(candidate) : self->clampedValue(candidate);
  self->requestUpdate();
}

void IntervalSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->adjustValue(event.value * self->smallStep);
}
#endif

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  ignoreConfirmRelease = ignoreConfirmRelease || mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignorePowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(true);
  }
  value = clampedValue(value);
#if CROSSINK_APP_CAP_TOUCH
  if (usesReaderSlider()) {
    applySharedUiTheme(app, uiTarget);
    app.on(ACTION_SLIDER, &IntervalSelectionActivity::onSliderEvent, this);
    app.on(ACTION_STEP, &IntervalSelectionActivity::onStepEvent, this);
    app.setScreen(&IntervalSelectionActivity::sliderScreen, this);
  }
#endif
  requestUpdate();
}

void IntervalSelectionActivity::onExit() {
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(false);
  }
#if CROSSINK_APP_CAP_TOUCH
  uiReady = false;
#endif
  Activity::onExit();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::drawStepHintLine(const int y, const StrId labelId, const int step) {
  char stepText[24];
  if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(stepText, sizeof(stepText), I18N.get(valueFormatId), static_cast<unsigned int>(step));
  } else {
    snprintf(stepText, sizeof(stepText), "%d", step);
  }
  char line[64];
  snprintf(line, sizeof(line), "%s %s", I18N.get(labelId), stepText);
  renderer.drawCenteredText(SMALL_FONT_ID, y, line, true);
}

void IntervalSelectionActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back, ignoreBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power, ignorePowerRelease)) {
    return;
  }
  if (ignoreConfirmRelease) {
    (void)InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                                   ignoreConfirmRelease);
    return;
  }

  if (showTouchHeaderBackButton && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (usesReaderSlider() && uiReady) {
    fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      sliderTapPending = snap.touchReleased && snap.touchX >= 0;
      const auto event = app.route(snap);
      sliderTapPending = false;
      if (event) {
        if (event.dragPermille >= 0 && snap.touchHeld) draggingSlider = true;
        if (app.invalidated()) requestUpdate();
        return;
      }
    }
    if (draggingSlider) {
      if (!snap.touchHeld) draggingSlider = false;
      return;
    }
  }
#endif

  int tx = 0;
  int ty = 0;
  const Rect touchScreen = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
#if CROSSINK_APP_CAP_TOUCH
  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;
  const bool useLegacyTouchBar = !usesReaderSlider();
  // Live drag on the slider: once a touch lands on the bar, the value follows the
  // finger until release. Runs before the Back/Confirm handlers because the release
  // of a drag can also register as a swipe (e.g. the left-edge rightward back
  // gesture) — the drag must consume it so it can't cancel or confirm the dialog.
  if (useLegacyTouchBar && mappedInput.isScreenTouchHeld(tx, ty)) {
    if (draggingBar || (ty >= barY - 20 && ty < barY + barHeight + 20 && tx >= barX && tx < barX + barWidth)) {
      draggingBar = true;
      const int range = std::max(1, maxValue - minValue);
      const int dragged =
          clampedValue(minValue + std::clamp(tx - barX, 0, barWidth - 1) * range / std::max(1, barWidth - 1));
      if (dragged != value) {
        value = dragged;
        requestUpdate();
      }
      return;
    }
  } else if (useLegacyTouchBar && draggingBar) {
    // Release frame of a drag: swallow the tap/swipe events it produced.
    draggingBar = false;
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY) && tapY >= barY - 20 && tapY < barY + barHeight + 20 && tapX >= barX &&
        tapX < barX + barWidth) {
      const int range = std::max(1, maxValue - minValue);
      value = tappedValue(minValue + (tapX - barX) * range / std::max(1, barWidth - 1));
      requestUpdate();
    }
    return;
  }
#endif

  // Cancel and Confirm act on touch-down. Unlike the adjustment controls, these
  // are terminal actions, so waiting for a release can make a perfectly still
  // tap feel ignored while the controller settles its release event.
  if (mappedInput.hasTouch() && mappedInput.wasScreenTouchDown(tx, ty)) {
    const auto actions = touchActionLayout(touchScreen);
    const int touchedAction = TouchActionButtons::indexAt(actions, tx, ty);
    if (touchedAction == 0) {
      // This activity closes on touch-down, so keep its matching release from
      // activating a settings row or gesture after the parent resumes.
      mappedInput.suppressCurrentTouchContact();
      setResult(IntervalResult{static_cast<uint32_t>(value)});
      finish();
      return;
    }
    if (touchedAction == 1) {
      mappedInput.suppressCurrentTouchContact();
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (useLegacyTouchBar && mappedInput.wasScreenTapped(tx, ty)) {
    if (ty >= barY - 20 && ty < barY + barHeight + 20 && tx >= barX && tx < barX + barWidth) {
      const int range = std::max(1, maxValue - minValue);
      value = tappedValue(minValue + (tx - barX) * range / std::max(1, barWidth - 1));
      requestUpdate();
      return;
    }

    if (mappedInput.hasTouch()) {
      for (int index = 0; index < 4; ++index) {
        const Rect stepRect = usesTextTouchStepControls() ? touchStepLabelRect(touchScreen, index)
                                                          : touchStepButtonRect(touchScreen, index);
        if (!contains(stepRect, tx, ty)) continue;
        constexpr int deltas[] = {-1, -1, 1, 1};
        const int step = (index == 0 || index == 3) ? largeStep : smallStep;
        adjustValue(deltas[index] * step);
        return;
      }

      return;
    }

    if (ty >= renderer.getScreenHeight() - 80) {
      if (tx < screenWidth / 3) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
      } else if (tx > screenWidth * 2 / 3) {
        setResult(IntervalResult{static_cast<uint32_t>(value)});
        finish();
      }
      return;
    }
  }
#endif

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });

  // On edge-button boards (X3, X4 Pro) the side buttons sit on the left/right edges of the screen rather
  // than as a vertical up/down rocker (X4), so BTN_UP is physically the left button and BTN_DOWN the right
  // one. Flip the large-step direction there so the left button decreases and the right button increases.
  const int upDelta = deviceHasEdgeSideButtons(gpio) ? -largeStep : largeStep;
  const int downDelta = deviceHasEdgeSideButtons(gpio) ? largeStep : -largeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustValue(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustValue(downDelta); });
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect touchScreen = safe;

#if CROSSINK_APP_CAP_TOUCH
  if (usesReaderSlider()) {
    uiReady = false;
    app.render();
    uiReady = true;

    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    TouchHeaderBackButton::draw(renderer, header, I18N.get(titleId), readerActivity);
    const auto actions = touchActionLayout(touchScreen);
    const char* labels[] = {tr(STR_CONFIRM), tr(STR_CANCEL)};
    TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
    renderer.displayBuffer();
    return;
  }
#endif

  // The Auto Page Turn picker has no touch back button. Keep its title in the
  // normal-height header rather than centering it in the taller touch-navigation
  // header, which puts the title against the divider.
  Rect header = showTouchHeaderBackButton ? TouchHeaderBackButton::headerRect(renderer, mappedInput)
                                          : TouchHeaderBackButton::standardHeaderRect(renderer);
  header.x = safe.x;
  header.width = safe.width;
  if (showTouchHeaderBackButton && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, I18N.get(titleId), readerActivity);
  } else {
    GUI.drawHeader(renderer, header, I18N.get(titleId), nullptr, readerActivity);
  }

  char formattedValue[32] = {};
  formatValue(formattedValue, sizeof(formattedValue));
  renderer.drawCenteredText(UI_12_FONT_ID, 90, formattedValue, true, EpdFontFamily::BOLD);

  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = (barWidth - 4) * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  if (mappedInput.hasTouch()) {
    if (usesTextTouchStepControls()) {
      char labels[4][12];
      snprintf(labels[0], sizeof(labels[0]), "%+d", -largeStep);
      snprintf(labels[1], sizeof(labels[1]), "%+d", -smallStep);
      snprintf(labels[2], sizeof(labels[2]), "%+d", smallStep);
      snprintf(labels[3], sizeof(labels[3]), "%+d", largeStep);
      for (int index = 0; index < 4; ++index) {
        const Rect rect = touchStepLabelRect(touchScreen, index);
        const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[index]);
        const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
        renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, labels[index]);
      }
    } else {
      auto drawButton = [&](const Rect& rect) {
        renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::White);
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
      };
      auto drawChevron = [&](const Rect& rect, const bool pointsRight, const bool doubleChevron) {
        const int centreY = rect.y + rect.height / 2;
        const int halfHeight = 12;
        const int firstX = rect.x + (doubleChevron ? 13 : 20);
        const int spacing = 14;
        const int chevronCount = doubleChevron ? 2 : 1;
        for (int i = 0; i < chevronCount; ++i) {
          const int x = firstX + i * spacing;
          if (pointsRight) {
            renderer.drawLine(x, centreY - halfHeight, x + 12, centreY, 2, true);
            renderer.drawLine(x + 12, centreY, x, centreY + halfHeight, 2, true);
          } else {
            renderer.drawLine(x + 12, centreY - halfHeight, x, centreY, 2, true);
            renderer.drawLine(x, centreY, x + 12, centreY + halfHeight, 2, true);
          }
        }
      };

      for (int index = 0; index < 4; ++index) {
        const Rect rect = touchStepButtonRect(touchScreen, index);
        drawButton(rect);
        drawChevron(rect, index >= 2, index == 0 || index == 3);
      }
    }

    const auto actions = touchActionLayout(touchScreen);
    const char* labels[] = {tr(STR_CONFIRM), tr(STR_CANCEL)};
    TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
  } else {
    // Two-line step hint: front buttons do the small step, side buttons the large step. Built from
    // separate label + value strings (rather than splitting one localized sentence) so the layout
    // doesn't depend on translators preserving a hidden separator.
    drawStepHintLine(barY + 30, StrId::STR_STEP_HINT_FRONT, smallStep);
    drawStepHintLine(barY + 52, StrId::STR_STEP_HINT_SIDE, largeStep);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, readerActivity);
  }

  renderer.displayBuffer();
}
