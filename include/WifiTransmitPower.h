#pragma once

#include <atomic>
#include <cstdint>

namespace WifiTransmitPower {
// ESP-IDF units are quarter-dBm; zero leaves the driver's default unchanged.
inline constexpr uint8_t TRANSMIT_POWER_LEVELS[] = {0, 60, 34, 20, 8};

namespace detail {
inline std::atomic<uint8_t> transmitPowerLimit{0};
}

inline void setLimit(uint8_t power) {
  for (const uint8_t supported : TRANSMIT_POWER_LEVELS) {
    if (power == supported) {
      detail::transmitPowerLimit.store(power, std::memory_order_relaxed);
      return;
    }
  }
  detail::transmitPowerLimit.store(0, std::memory_order_relaxed);
}

inline uint8_t getLimit() { return detail::transmitPowerLimit.load(std::memory_order_relaxed); }
}  // namespace WifiTransmitPower
