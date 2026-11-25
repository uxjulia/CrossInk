#pragma once

#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A compact 12-hour time editor for the frontlight schedule. It stores the
// same minute-of-day values as the schedule engine, while presenting tap-target
// fields for hour, minutes, and AM/PM.
class FrontlightTimePickerActivity final : public Activity {
 public:
  FrontlightTimePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                               uint16_t initialTimeOfDay);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
#ifdef SIMULATOR
  friend struct ScreenCalibrationSmokeTest;
#endif
  enum class Field : uint8_t { Hour, Minute, Period, Count };

  StrId titleId;
  uint16_t initialTimeOfDay;
  uint8_t hour12 = 12;
  uint8_t minute = 0;
  bool isPm = true;
  Field activeField = Field::Hour;
  ButtonNavigator buttonNavigator;
  freeink::ui::InteractionBuffer<48> keyboardInteractions;
  freeink::ui::TouchHoldRouter keyboardTouchRouter;
  std::atomic<bool> keyboardInteractionsReady{false};
  uint8_t numericEntry = 0;
  uint8_t numericEntryDigits = 0;

  void adjustActiveField(int delta);
  void selectNextField(int delta);
  void complete();
  bool selectFieldAt(int x, int y, bool togglePeriod);
  void clearNumericEntry();
  void enterDigit(uint8_t digit);
  void handleKeyboardValue(int16_t value);
};
