#pragma once
#include <Arduino.h>
namespace freeink {
struct PowerManager {
  static void powerDownRailsForSleep() {}
  static void waitForPowerButtonRelease() { ++power_test::releaseWaits; }
  static void armPowerButtonWakeup() {}
};
}  // namespace freeink
