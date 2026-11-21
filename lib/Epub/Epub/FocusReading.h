#pragma once

#include <algorithm>
#include <cstddef>

namespace FocusReading {
// Change the identity whenever the prefix rule changes so cached layouts rebuild.
constexpr unsigned LAYOUT_IDENTITY = 0x46524301u;

constexpr std::size_t boldPrefixLength(const std::size_t wordLength) {
  if (wordLength <= 3) return std::min<std::size_t>(wordLength, 1);
  return std::min<std::size_t>(wordLength / 2 + wordLength % 2, 9);
}
}  // namespace FocusReading
