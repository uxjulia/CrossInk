#pragma once

#include <BatteryEstimate.h>

// Plain data for RTC_NOINIT memory. Explicit validation is required even after a
// sleep wake: cold boot, flashing, or a different firmware can leave stale bytes.
struct BatteryEstimateRetention {
  uint32_t marker;
  BatteryEstimate::State state;
  uint64_t sampledAtMs;
  uint32_t checksum;

  void save(const BatteryEstimate& estimate, uint64_t nowMs) {
    marker = 0;
    state = estimate.state();
    sampledAtMs = nowMs;
    checksum = check();
    marker = MAGIC;
  }

  bool restore(BatteryEstimate& estimate, uint64_t nowMs) const {
    constexpr uint64_t MAX_AGE_MS = 7ull * 24 * 60 * 60 * 1000;
    return marker == MAGIC && checksum == check() && nowMs >= sampledAtMs && nowMs - sampledAtMs <= MAX_AGE_MS &&
           estimate.restore(state);
  }

 private:
  static constexpr uint32_t MAGIC = 0xBA770103;
  uint32_t check() const {
    uint32_t value = MAGIC;
    for (const uint32_t field :
         {static_cast<uint32_t>(state.percentQ8), state.settlingMs, static_cast<uint32_t>(state.power),
          static_cast<uint32_t>(sampledAtMs), static_cast<uint32_t>(sampledAtMs >> 32)})
      value = (value ^ field) * 16777619u;
    return value;
  }
};
