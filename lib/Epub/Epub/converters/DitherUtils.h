#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3).
// Stateless - works correctly with any pixel processing order.
// highQuality=true tunes for the factory LUT (lighter palette → soft-shoulder
// darkening + raised T12/T23 thresholds). highQuality=false is the legacy
// CrossInk path tuned for the differential LUT (uniform 64/128/192 thresholds).
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y, bool highQuality) {
  int g = gray;
  if (highQuality) {
    // Factory LUT renders palette levels physically lighter than the
    // differential LUT. Ramp a -12 offset in across gray [0, 64], flat above,
    // so mid/bright pixels land back at perceived mid-gray without crushing
    // shadows.
    int offset = (g < 64) ? g * 12 / 64 : 12;
    g -= offset;
  }

  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = g + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (highQuality) {
    // T12 raised from 128 to 150 so mid-bright source pixels (sRGB 150–170)
    // land in the palette 1 / palette 2 dither zone, producing ~50% perceived
    // reflectance via 57/43 mixing — the perceptual mid-gray that factory LUT
    // can't reach with palette 2 alone (~70% reflectance).
    // T23 raised from 192 to 210 to keep mid-bright pixels (sRGB 180–210) from
    // blowing out to pure white after the soft-shoulder offset is applied.
    if (adjusted < 150) return 1;
    if (adjusted < 210) return 2;
  } else {
    if (adjusted < 128) return 1;
    if (adjusted < 192) return 2;
  }
  return 3;
}
