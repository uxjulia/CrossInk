#pragma once
#include <cstdint>
#include <initializer_list>
#include <mutex>
namespace power_test {
inline unsigned long now = 1;
inline int frequency = 160;
inline bool frequencyFails = false;
inline unsigned gaugeReads = 0;
inline uint16_t percentage = 80;
inline uint16_t batteryMv = 4065;
inline bool charging = false;
inline bool chargeComplete = false;
inline int16_t chargerStatus = 0;
inline uint64_t rtcMs = 100000;
inline int wakeupCause = 0;
inline bool gaugeFails = false;
inline bool calibrationPending = false;
inline bool usbPowered = false;
inline bool pressOnWake = false;
inline bool powerPressed = false;
inline bool lightOn = true;
inline bool touchPreparedWithLightOff = false;
inline unsigned chargerSetups = 0;
inline unsigned lightSleeps = 0;
inline unsigned releaseWaits = 0;
inline unsigned wakeCleanup = 0;
inline bool runtimeInterruptEnabled = true;
inline bool unsafeLevelWake = false;
inline bool interruptDisableFails = false;
inline bool pressAfterDelay = false;
inline unsigned timerWakesBeforeAction = 0;
}  // namespace power_test
inline unsigned long millis() { return power_test::now; }
inline void delay(unsigned long ms) {
  power_test::now += ms;
  if (power_test::pressAfterDelay) power_test::powerPressed = true;
}
inline int getCpuFrequencyMhz() { return power_test::frequency; }
inline bool setCpuFrequencyMhz(int value) {
  if (power_test::frequencyFails) return false;
  power_test::frequency = value;
  return true;
}
constexpr int INPUT = 0;
inline void pinMode(int, int) {}

constexpr int LOW = 0;
inline int digitalRead(int) { return power_test::powerPressed ? LOW : 1; }
