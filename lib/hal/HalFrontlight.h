#pragma once

#include <FrontlightManager.h>

// Thin HAL over the SDK's FrontlightManager (activities never touch SDK
// classes directly). Inert on boards without a frontlight, so it is always
// safe to call. State (brightness / warmth / on) is owned by the SDK manager;
// persistence policy lives in CrossPointSettings and the frontlight panel.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  // Detect and initialize the driver before settings validate light actions.
  // Does not apply saved light state; safe to call again when restoring it.
  void initializeHardware();

  // Apply the given persisted state, initializing the driver if needed.
  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  // Brightness as 0-100 (total light on a warm/cool pair). Setting a value
  // while off leaves the light off; it is applied on the next on().
  void setBrightness(uint8_t percent);
  // Warm/cool mix, 0 = cool .. 100 = warm. No-op on single-channel boards.
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

 private:
  HalFrontlight() = default;

  FrontlightManager manager;
  bool initialized = false;
  // The SDK manager folds "off" into brightness 0; keep the user's level and
  // the on/off state separate so toggling restores the previous level.
  uint8_t lastBrightness = 60;
  bool lit = false;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
