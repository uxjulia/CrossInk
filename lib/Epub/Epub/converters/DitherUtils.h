#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  // Output palette {0, 85, 170, 255}. EPUB image pages render on the factory
  // LUT (EpubReaderActivity: useFactoryGray), where palette levels are
  // physically lighter than on the differential LUT used upstream. To get
  // brightness roughly equivalent to upstream's differential rendering, we
  // raise T12 and T23 to push more pixels into the darker palette indices:
  //   T01 = 43  — calibrated shadow boundary (linear midpoint, unchanged)
  //   T12 = 170 — widens palette 1 band so mid-bright pixels (sRGB 128–170)
  //               render as gray 85 instead of gray 170
  //   T23 = 235 — narrows the white band so only true highlights come out
  //               pure white; mid-light tones stay at gray 170
  if (adjusted < 43) return 0;
  if (adjusted < 170) return 1;
  if (adjusted < 235) return 2;
  return 3;
}
