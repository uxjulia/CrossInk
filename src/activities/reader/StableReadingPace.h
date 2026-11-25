#pragma once

#include <cmath>
#include <cstdint>

// Time and source-text distance from accepted forward reads. Source units are
// the same words/characters used by the book's stable pages, never screenfuls.
class StableReadingPace {
  double units = 0;
  uint32_t elapsedSeconds = 0;
  uint16_t samples = 0;

 public:
  void record(uint32_t seconds, double distance) {
    if (seconds == 0 || !std::isfinite(distance) || distance <= 0 || samples == UINT16_MAX ||
        elapsedSeconds > UINT32_MAX - seconds)
      return;
    units += distance;
    elapsedSeconds += seconds;
    ++samples;
  }

  bool estimate(double remainingUnits, uint32_t& seconds) const {
    seconds = 0;
    if (samples < 3 || units <= 0 || !std::isfinite(remainingUnits) || remainingUnits <= 0) return false;
    const double estimate = remainingUnits * elapsedSeconds / units;
    seconds = estimate >= UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(estimate + 0.5);
    return seconds > 0;
  }
};
