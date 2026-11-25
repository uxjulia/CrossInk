#pragma once

#include <cstdint>

namespace textDarkness {
// Coverage is 0=transparent, 1=light gray, 2=dark gray, 3=solid ink.
// Keep a gray edge at both stronger settings; never enlarge the glyph.
constexpr uint8_t coverage(uint8_t ink, uint8_t level) {
  if (level == 1 && ink == 1) return 2;
  if (level == 2 && ink != 0 && ink != 3) return ink + 1;
  return ink;
}
}  // namespace textDarkness
