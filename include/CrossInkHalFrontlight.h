#pragma once

#ifdef SIMULATOR

#include <cstdint>

// The simulator has no physical light. Keep the same stateful HAL contract so
// frontlight UI code can compile and be exercised without SDK PWM hardware.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() {
    static HalFrontlight instance;
    return instance;
  }

  void initializeHardware() {}
  void begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
    lastBrightness = brightness > 100 ? 100 : brightness;
    lastWarmth = warmth > 100 ? 100 : warmth;
    lit = on;
  }
  constexpr bool present() const {
#ifdef SIMULATOR_DEVICE_X4_PRO
    return true;
#else
    return false;
#endif
  }
  constexpr bool hasColorTemperature() const {
#ifdef SIMULATOR_DEVICE_X4_PRO
    return true;
#else
    return false;
#endif
  }
  void setBrightness(const uint8_t percent) { lastBrightness = percent > 100 ? 100 : percent; }
  void setWarmth(const uint8_t percent) { lastWarmth = percent > 100 ? 100 : percent; }
  void setOn(const bool on) { lit = on; }
  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return lastWarmth; }
  bool isOn() const { return lit; }

 private:
  uint8_t lastBrightness = 60;
  uint8_t lastWarmth = 50;
  bool lit = false;
};

#define Frontlight HalFrontlight::getInstance()

#else

#include <HalFrontlight.h>

#endif
