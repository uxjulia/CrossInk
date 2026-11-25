#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// XTC progress is the one-based page number divided by the page count.
// Pick the nearest page and keep 100% on the last bitmap, not the end screen.
inline uint32_t xtcPageForPercent(const float percent, const uint32_t pageCount) {
  if (pageCount == 0 || !std::isfinite(percent)) return 0;
  const double page = std::round(static_cast<double>(std::clamp(percent, 0.0f, 100.0f)) * pageCount / 100.0);
  return static_cast<uint32_t>(std::clamp(page, 1.0, static_cast<double>(pageCount))) - 1;
}
