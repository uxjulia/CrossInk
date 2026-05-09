#include "EpubReaderAutoPageTurnIntervalActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMinSeconds = 5;
constexpr int kMaxSeconds = 120;
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 5;

int clampSeconds(const int seconds) { return std::clamp(seconds, kMinSeconds, kMaxSeconds); }
}  // namespace

void EpubReaderAutoPageTurnIntervalActivity::onEnter() {
  Activity::onEnter();
  seconds = clampSeconds(seconds);
  requestUpdate();
}

void EpubReaderAutoPageTurnIntervalActivity::adjustSeconds(const int delta) {
  seconds = clampSeconds(seconds + delta);
  requestUpdate();
}

void EpubReaderAutoPageTurnIntervalActivity::loop() {
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
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
    setResult(AutoPageTurnResult{static_cast<uint32_t>(seconds)});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustSeconds(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustSeconds(kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustSeconds(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustSeconds(-kLargeStep); });
}

void EpubReaderAutoPageTurnIntervalActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_AUTO_TURN_INTERVAL_SECONDS), true, EpdFontFamily::BOLD);

  const std::string secondsText = std::to_string(seconds);
  renderer.drawCenteredText(UI_12_FONT_ID, 90, secondsText.c_str(), true, EpdFontFamily::BOLD);

  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int fillWidth = (barWidth - 4) * (seconds - kMinSeconds) / (kMaxSeconds - kMinSeconds);
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, tr(STR_AUTO_TURN_STEP_HINT), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
