#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "lib/EpdFont/CpfontStrike.h"
#include "lib/GfxRenderer/GlyphBitmap.h"
#include "lib/GfxRenderer/ScaledGlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

using glyphBitmap::Plane;

// Logical placements used by the renderer: unrotated text, and side-button
// labels rotated 90 degrees clockwise (glyph x runs up, glyph y runs right).
constexpr std::array<std::array<int, 4>, 2> AXES = {{{1, 0, 0, 1}, {0, -1, 1, 0}}};

std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {PANEL_WIDTH - 1 - y, x};
    default:
      return {x, y};
  }
}

// Logical rectangle the renderer clips to; the glyph-local clip is derived from it.
struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

void compare(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x, int y,
             int originY, int rows, Rect clip, bool absolute = false, uint8_t darkness = 0) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << rotation << ',' << twoBit << ',' << static_cast<int>(plane)
                                    << ',' << state << " size=" << width << 'x' << height << " at=" << x << ',' << y
                                    << " band=" << originY << ',' << rows);
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + 0x1b);
  // Nonuniform background and guards verify transparent pixels and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      bool draw = false;
      bool black = state;
      if (twoBit) {
        int ink = (bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3;
        if (plane != Plane::BW && darkness == 1 && ink == 1)
          ink = 2;
        else if (plane != Plane::BW && darkness == 2 && ink > 0 && ink < 3)
          ++ink;
        const int value = 3 - ink;
        if (plane == Plane::BW) draw = value < 3;
        if (plane == Plane::GrayLSB) draw = value == 1;
        if (plane == Plane::GrayMSB) draw = value == 1 || value == 2;
        if (plane != Plane::BW) {
          black = false;
          if (absolute) {
            draw = value != 3;
            const int level = state ? value : 3 - value;
            black = !(level & (plane == Plane::GrayMSB ? 2 : 1));
          }
        }
      } else {
        draw = (bitmap[source / 8] >> (7 - source % 8)) & 1;
      }
      if (!draw) continue;
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      if (black)
        byte &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
      else
        byte |= 0x80 >> (px % 8);
    }
  }

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);

  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  glyphBitmap::draw(bitmap.data(), width, height, twoBit, plane, state, target, local, absolute, darkness);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsRotationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (bool twoBit : {false, true}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int width : {1, 3, 7, 16, 31}) {
              for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
                for (const auto [origin, rows] :
                     {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows,
                          {-20, -20, 60, 60});
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows, {2, 3, 18, 20});
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EmptyAndFullyClippedGlyphsDoNotWrite) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      compare(orientation, rotation, true, Plane::BW, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
    }
  }
}

TEST(GlyphBitmap, AbsoluteCoveragePreservesTransparencyAndMatchesAllOrientations) {
  for (int orientation = 0; orientation < 4; ++orientation)
    for (int rotation = 0; rotation < 2; ++rotation)
      for (bool twoBit : {false, true})
        for (bool black : {false, true})
          for (Plane plane : {Plane::GrayLSB, Plane::GrayMSB})
            for (int width : {1, 3, 7, 16, 31})
              for (const auto [origin, rows] : {std::pair{0, 32}, std::pair{7, 11}})
                compare(orientation, rotation, twoBit, plane, black, width, 13, 9, 11, origin, rows, {2, 3, 28, 30},
                        true);
}

TEST(GlyphBitmap, DarknessPreservesClippingRotationsTransparencyAndMonochrome) {
  for (uint8_t darkness : {0, 1, 2, 255}) {
    for (bool absolute : {false, true}) {
      for (int orientation = 0; orientation < 4; ++orientation) {
        for (int rotation = 0; rotation < 2; ++rotation) {
          for (bool twoBit : {false, true}) {
            for (bool state : {false, true}) {
              for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
                compare(orientation, rotation, twoBit, plane, state, 31, 13, 9, 11, 7, 11, {2, 3, 28, 25}, absolute,
                        darkness);
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, StrongerTextRetainsGrayEdgesAndSolidCores) {
  for (uint8_t level : {0, 1, 2}) {
    EXPECT_EQ(textDarkness::coverage(0, level), 0);
    EXPECT_EQ(textDarkness::coverage(3, level), 3);
    EXPECT_LT(textDarkness::coverage(1, level), 3);
    for (uint8_t ink = 1; ink < 3; ++ink) EXPECT_GE(textDarkness::coverage(ink, level), ink);
  }
}

TEST(ScaledGlyphBitmap, EmitsBothEdgeShadesWithMatchingGrayscaleBaseAndPreservesMonochrome) {
  // At 2x, one black source pixel to the right of one white source pixel
  // becomes [white, light, dark, dark] on both destination rows.
  const uint8_t bitmap[] = {0x30};
  const auto render = [&](Plane plane, bool antialias, bool twoBit = true, bool absolute = false, bool black = true,
                          uint8_t darkness = 0) {
    const uint8_t background = plane == Plane::BW || absolute ? 0xff : 0x00;
    std::array buffer{background, background};
    const glyphBitmap::Target target{buffer.data(), 8, 1, 0, 2, {0, 0, 1, 0, 0, 1}};
    const uint8_t binaryBitmap[] = {0x40};
    glyphBitmap::drawScaled(twoBit ? bitmap : binaryBitmap, 2, 1, 512, twoBit, plane, black, target, {0, 0, 4, 2},
                            antialias, absolute, darkness);
    EXPECT_EQ(buffer[0], buffer[1]);
    return buffer[0];
  };
  EXPECT_EQ(render(Plane::BW, false), 0xcf);      // Old threshold: only the two dark pixels.
  EXPECT_EQ(render(Plane::BW, true), 0x8f);       // Matching base retains the light edge too.
  EXPECT_EQ(render(Plane::GrayMSB, true), 0x70);  // Overlay includes both edge shades.
  EXPECT_EQ(render(Plane::GrayLSB, true), 0x30);  // Dark edge only.
  EXPECT_EQ(render(Plane::GrayMSB, true, false), 0);
  EXPECT_EQ(render(Plane::GrayLSB, true, false), 0);
  EXPECT_EQ(render(Plane::BW, true, false), 0xcf);
  EXPECT_EQ(render(Plane::GrayMSB, true, true, true), 0xcf);
  EXPECT_EQ(render(Plane::GrayLSB, true, true, true), 0xbf);
  EXPECT_EQ(render(Plane::GrayMSB, true, true, true, false), 0xbf);
  EXPECT_EQ(render(Plane::GrayLSB, true, true, true, false), 0xcf);
  EXPECT_EQ(render(Plane::GrayMSB, true, true, false, true, 2), 0x40);
  EXPECT_EQ(render(Plane::GrayLSB, true, true, false, true, 2), 0x40);
}

TEST(ScaledGlyphBitmap, TiledRenderingMatchesWholePlaneAcrossRotationAndClipping) {
  // Packed 2bpp rows are not padded to a byte boundary.
  const uint8_t bitmap[] = {0x37, 0x6b, 0xf0, 0xe4};
  constexpr int width = 5;
  constexpr int height = 3;
  for (const auto& frame : {glyphBitmap::Frame{-3, 4, 1, 0, 0, 1}, glyphBitmap::Frame{9, 13, 0, -1, 1, 0},
                            glyphBitmap::Frame{23, 12, -1, 0, 0, -1}}) {
    for (const auto mode : {Plane::BW, Plane::GrayMSB, Plane::GrayLSB}) {
      const glyphBitmap::Clip clip{1, 1, 24, 8};
      std::array<uint8_t, 16 * 3> whole;
      whole.fill(0xa5);
      auto strips = whole;
      const glyphBitmap::Target full{whole.data(), 24, 3, 0, 16, frame};
      glyphBitmap::drawScaled(bitmap, width, height, 798, true, mode, true, full, clip, true, false, 0);
      for (const auto& [start, rows] : {std::pair{0, 3}, std::pair{3, 7}, std::pair{10, 6}}) {
        const glyphBitmap::Target strip{strips.data() + start * 3, 24, 3, start, rows, frame};
        glyphBitmap::drawScaled(bitmap, width, height, 798, true, mode, true, strip, clip, true, false, 0);
      }
      EXPECT_EQ(whole, strips);
    }
  }
}

namespace {
struct CpfontFixture {
  static constexpr uint32_t intervals = 4096, dataOffset = 96;
  static constexpr uint32_t recordOffset = dataOffset + intervals * 12;
  static constexpr uint32_t bitmapOffset = recordOffset + intervals * 16;
  static constexpr uint32_t secondStyleOffset = bitmapOffset + 10;
  std::vector<uint8_t> data = std::vector<uint8_t>(secondStyleOffset + 32);
  std::vector<std::pair<uint32_t, size_t>> reads;

  void put(unsigned offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }

  CpfontFixture() {
    std::memcpy(data.data(), "CPFONT\0\0", 8);
    data[8] = 4;
    data[10] = 1;
    data[12] = 2;
    put(36, intervals);
    put(40, intervals);
    put(56, dataOffset);
    data[64] = 1;
    put(88, secondStyleOffset);
    for (unsigned i = 0; i < intervals; ++i) {
      const auto off = dataOffset + 12 * i;
      put(off, 32 + 2 * i);
      put(off + 4, 32 + 2 * i);
      put(off + 8, i);
    }
    data[recordOffset] = 3;
    data[recordOffset + 1] = 5;
    data[recordOffset + 8] = 4;  // Packed width is not a multiple of 4.
    put(recordOffset + 12, 3);
  }

  cpfontStrike::Reader reader() {
    return {this, static_cast<uint32_t>(data.size()), [](void* ptr, uint32_t offset, void* out, size_t length) {
              auto& f = *static_cast<CpfontFixture*>(ptr);
              f.reads.emplace_back(offset, length);
              std::memcpy(out, f.data.data() + offset, length);
              return true;
            }};
  }
};
}  // namespace

TEST(CpfontStrike, FindsSparsePackedGlyphWithBoundedSeeksAndNoBitmapReads) {
  CpfontFixture f;
  cpfontStrike::Glyph result{};
  ASSERT_TRUE(cpfontStrike::find(f.reader(), 0, 32, result));
  EXPECT_EQ(result.offset, CpfontFixture::bitmapOffset + 3);
  EXPECT_EQ(result.bytes, 4);
  EXPECT_EQ(result.width, 3);
  EXPECT_EQ(result.height, 5);
  EXPECT_LE(f.reads.size(), 17);  // header + two TOCs + thirteen interval probes + one glyph record
  for (const auto& [offset, bytes] : f.reads) {
    EXPECT_LE(offset + bytes, CpfontFixture::bitmapOffset);
  }
  f.reads.clear();
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 33, result));
  EXPECT_LE(f.reads.size(), 16);
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 1, 32, result));
}

TEST(CpfontStrike, RefusesAdjacentStyleOrShaperAndMalformedPackedLength) {
  CpfontFixture f;
  cpfontStrike::Glyph result{};
  f.put(CpfontFixture::recordOffset + 12, 7);  // Four bytes would reach the next style.
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
  f.put(CpfontFixture::recordOffset + 12, UINT32_MAX);
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
  f.put(CpfontFixture::recordOffset + 12, 3);
  f.put(60, CpfontFixture::bitmapOffset + 4);  // Valid shaping offset but overlaps this glyph.
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
  f.put(60, 0);
  f.data[CpfontFixture::recordOffset + 8] = 3;
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
  f.data[CpfontFixture::recordOffset] = 255;
  f.data[CpfontFixture::recordOffset + 1] = 255;
  f.data[CpfontFixture::recordOffset + 8] = 0x81;
  f.data[CpfontFixture::recordOffset + 9] = 0x3f;
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
  f.put(56, UINT32_MAX);
  EXPECT_FALSE(cpfontStrike::find(f.reader(), 0, 32, result));
}

TEST(CpfontStrike, ChoosesNearestLargerOrdinarySizeWithExistingTieBreak) {
  const auto best = [](uint8_t body, uint16_t scale, std::initializer_list<uint8_t> installed) {
    uint8_t selected = 0;
    for (const uint8_t points : installed) {
      if (cpfontStrike::preferPoints(body, cpfontStrike::targetPoints(body, scale), selected, points))
        selected = points;
    }
    return selected;
  };
  EXPECT_EQ(best(14, 960, {12, 14, 18, 16, 24}), 24);  // C3 cap target is limited to 44 pt.
  EXPECT_EQ(best(14, 960, {16, 12, 18, 14}), 18);
  EXPECT_EQ(best(14, 960, {12, 14}), 0);
  EXPECT_EQ(best(14, 512, {30, 26}), 30);
  EXPECT_EQ(best(14, 512, {120, 64, 22}), 22);
}
