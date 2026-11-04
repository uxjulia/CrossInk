#pragma once

#include <cstdint>

// Four bytes in RTC memory retain an accepted USB-sleep wake through restart.
// No initializer: RTC_NOINIT storage must also tolerate cold-boot garbage.
struct UsbSleepWakeToken {
  uint32_t marker;

  void arm() { marker = MAGIC; }
  bool consume(const bool softwareReset) {
    const bool requested = softwareReset && marker == MAGIC;
    marker = 0;
    return requested;
  }

 private:
  static constexpr uint32_t MAGIC = 0xC1EAB027;
};
