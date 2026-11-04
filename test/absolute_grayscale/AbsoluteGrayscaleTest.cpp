#include <gtest/gtest.h>

#include "lib/Epub/Epub/converters/DitherUtils.h"
#include "lib/GfxRenderer/BitmapHelpers.h"

namespace {
thread_local int rowAllocationToFail = -1;
}

// Fail one row allocation without changing production allocator APIs.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (rowAllocationToFail >= 0 && rowAllocationToFail-- == 0) return nullptr;
  return ::operator new[](size);
}

TEST(AbsoluteGrayscale, DitherersReportScratchAllocationFailure) {
  rowAllocationToFail = 0;
  AtkinsonDitherer failedAtkinson(8);
  rowAllocationToFail = -1;
  EXPECT_FALSE(failedAtkinson.isValid());

  rowAllocationToFail = 0;
  Atkinson1BitDitherer failedOneBit(8);
  rowAllocationToFail = -1;
  EXPECT_FALSE(failedOneBit.isValid());

  rowAllocationToFail = 0;
  FloydSteinbergDitherer failedFloyd(8);
  rowAllocationToFail = -1;
  EXPECT_FALSE(failedFloyd.isValid());

  EXPECT_FALSE(AtkinsonDitherer(0).isValid());
  EXPECT_FALSE(Atkinson1BitDitherer(-1).isValid());
  EXPECT_FALSE(FloydSteinbergDitherer(0).isValid());

  AtkinsonDitherer atkinson(8);
  Atkinson1BitDitherer oneBit(8);
  FloydSteinbergDitherer floyd(8);
  EXPECT_TRUE(atkinson.isValid());
  EXPECT_TRUE(oneBit.isValid());
  EXPECT_TRUE(floyd.isValid());
}

TEST(AbsoluteGrayscale, FullPlanesIncludeBlackWhiteAndBothGrayLevels) {
  uint8_t planes[2] = {0xff, 0xff};
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned x = 0; x < 8; ++x) {
      const auto pixel = grayPlanePixel(x % 4, p == 1, true);
      ASSERT_TRUE(pixel.write);
      if (pixel.black)
        planes[p] &= ~(0x80 >> x);
      else
        planes[p] |= 0x80 >> x;
    }
  }
  EXPECT_EQ(planes[0], 0x55);  // black/dark/light/white = 0/1/0/1
  EXPECT_EQ(planes[1], 0x33);  // black/dark/light/white = 0/0/1/1
}

TEST(AbsoluteGrayscale, OverlayStillLeavesBlackAndWhiteToTheBase) {
  uint8_t planes[2] = {0, 0};
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned x = 0; x < 8; ++x) {
      const auto pixel = grayPlanePixel(x % 4, p == 1, false);
      if (x % 4 == 0 || x % 4 == 3) EXPECT_FALSE(pixel.write);
      if (pixel.write && !pixel.black) planes[p] |= 0x80 >> x;
    }
  }
  EXPECT_EQ(planes[0], 0x44);
  EXPECT_EQ(planes[1], 0x66);
}

TEST(AbsoluteGrayscale, ImageQuantizersRetainFourEvenLevels) {
  for (uint8_t level = 0; level < 4; ++level) {
    AtkinsonDitherer atkinson(1, ImageToneProfile::Even);
    FloydSteinbergDitherer floyd(1, ImageToneProfile::Even);
    EXPECT_EQ(atkinson.processPixel(level * 85, 0), level);
    EXPECT_EQ(floyd.processPixel(level * 85, 0), level);
  }
  AtkinsonDitherer overlay(1);
  EXPECT_EQ(overlay.processPixel(85, 0), 2);
}

TEST(AbsoluteGrayscale, PiccoImageMatchesAbsoluteCoverAcrossTheRamp) {
  AtkinsonDitherer picco(256, ImageToneProfile::PiccoCover), reference(256, ImageToneProfile::Even);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 256; ++x) ASSERT_EQ(picco.processPixel(x, x), reference.processPixel(x, x));
    picco.nextRow();
    reference.nextRow();
  }
  EXPECT_STREQ(coverToneSuffix(ImageToneProfile::PiccoCover), "_picco_image");
}

TEST(AbsoluteGrayscale, NonDitheredPngQuantizationUsesFourEvenLevels) {
  EXPECT_EQ(quantizeGrayTo4Level(0), 0);
  EXPECT_EQ(quantizeGrayTo4Level(63), 0);
  EXPECT_EQ(quantizeGrayTo4Level(64), 1);
  EXPECT_EQ(quantizeGrayTo4Level(127), 1);
  EXPECT_EQ(quantizeGrayTo4Level(128), 2);
  EXPECT_EQ(quantizeGrayTo4Level(191), 2);
  EXPECT_EQ(quantizeGrayTo4Level(192), 3);
  EXPECT_EQ(quantizeGrayTo4Level(255), 3);
}
