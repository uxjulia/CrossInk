#pragma once

#include "GlyphBitmap.h"

namespace glyphBitmap {

constexpr int scaledExtent(int source, int scale256) { return (source * scale256 + 255) / 256; }

// Bilinearly sample the original glyph at the centres of the destination
// pixels. Quantizing once to four coverage levels also preserves the old
// half-coverage threshold when only levels 2 and 3 are painted in B/W mode.
inline void drawScaled(const uint8_t* bitmap, int width, int height, int scale256, bool twoBit, Plane plane, bool state,
                       const Target& target, Clip clip, bool grayscaleBase, bool absolute, uint8_t darkness) {
  if (!bitmap || width <= 0 || height <= 0 || scale256 <= 0 || scale256 > 4096) return;
  // Intentionally monochrome faces stay monochrome.
  if (plane != Plane::BW && !twoBit) return;
  auto [levels, clearLevels] = inkPaint(true, plane, state, absolute, darkness);
  if (plane == Plane::BW && (!grayscaleBase || !twoBit)) levels = 0x0c;

  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, scaledExtent(width, scale256));
  clip.bottom = std::min(clip.bottom, scaledExtent(height, scale256));
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  const auto sample = [&](int x, int y) -> int {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    const int pos = y * width + x;
    if (twoBit) return (bitmap[pos >> 2] >> (6 - (pos & 3) * 2)) & 3;
    return (bitmap[pos >> 3] & (0x80 >> (pos & 7))) ? 3 : 0;
  };
  const auto& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int row = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  const int32_t step = (256 << 16) / scale256;
  const int32_t origin = step / 2 - (1 << 15);
  for (int dy = clip.top; dy < clip.bottom; ++dy, row += stepY) {
    const int32_t sy = origin + dy * step;
    const int y0 = sy >> 16;
    const int fy = (sy >> 8) & 0xff;
    int destination = row;
    for (int dx = clip.left; dx < clip.right; ++dx, destination += stepX) {
      const int32_t sx = origin + dx * step;
      const int x0 = sx >> 16;
      const int fx = (sx >> 8) & 0xff;
      const int upper = sample(x0, y0) * (256 - fx) + sample(x0 + 1, y0) * fx;
      const int lower = sample(x0, y0 + 1) * (256 - fx) + sample(x0 + 1, y0 + 1) * fx;
      const uint8_t ink = (upper * (256 - fy) + lower * fy + (1 << 15)) >> 16;
      paint(target.buffer, destination, ink, levels, clearLevels);
    }
  }
}

}  // namespace glyphBitmap
