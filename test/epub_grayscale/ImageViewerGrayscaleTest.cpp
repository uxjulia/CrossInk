#include <gtest/gtest.h>
class HalDisplay {
 public:
  enum { FAST_REFRESH };
};
#include "ImageViewerGrayscale.h"

TEST(ImageViewerGrayscale, CompletePassUsesDirectAndRebuildsBwChrome) {
  GfxRenderer r(32, 32);
  int calls = 0;
  ASSERT_TRUE(renderImageViewerGrayscale(r, [&](bool grayscale, bool chrome) {
    EXPECT_TRUE(grayscale);
    EXPECT_TRUE(chrome);
    r.clearScreen(r.mode == GfxRenderer::BW ? 0xa5 : r.mode == GfxRenderer::GRAYSCALE_LSB ? 0x55 : 0x33);
    ++calls;
    return true;
  }));
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(r.lsb[0], 0x55);
  EXPECT_EQ(r.msb[0], 0x33);
  EXPECT_EQ(r.bw[0], 0xa5);
  EXPECT_EQ(r.events, (std::vector<std::string>{"direct", "gray", "cleanup"}));
  EXPECT_EQ(r.mode, GfxRenderer::BW);
}
TEST(ImageViewerGrayscale, SeparateAbsoluteAndOverlayFallbacksKeepTheirEncoding) {
  for (bool absolute : {true, false}) {
    GfxRenderer r(32, 32);
    r.directSupported = false;
    r.absoluteSupported = absolute;
    ASSERT_TRUE(renderImageViewerGrayscale(r, [&](bool grayscale, bool chrome) {
      EXPECT_TRUE(grayscale);
      EXPECT_EQ(chrome, absolute || r.mode == GfxRenderer::BW);
      if (r.mode != GfxRenderer::BW) EXPECT_EQ(r.bw[0], absolute ? 0xff : 0x00);
      return true;
    }));
    EXPECT_EQ(r.events.front(), absolute ? "absolute" : "combined-base");
    EXPECT_EQ(r.events.back(), "cleanup");
  }
}
TEST(ImageViewerGrayscale, UnsupportedOrInvertedDisplayUsesDitheredMono) {
  for (bool inverted : {false, true}) {
    GfxRenderer r;
    r.inverted = inverted;
    r.absoluteSupported = false;
    r.supported = inverted;
    int calls = 0;
    ASSERT_TRUE(renderImageViewerGrayscale(r, [&](bool grayscale, bool chrome) {
      EXPECT_FALSE(grayscale);
      EXPECT_TRUE(chrome);
      ++calls;
      return true;
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(r.events, (std::vector<std::string>{"cleanup", "display"}));
  }
}
TEST(ImageViewerGrayscale, EveryReplayFailureCancelsAndDrawsMonoRatherThanLeavingAPlane) {
  for (int failure = 2; failure <= 4; ++failure) {
    GfxRenderer r(32, 32);
    int calls = 0;
    ASSERT_TRUE(renderImageViewerGrayscale(r, [&](bool grayscale, bool chrome) {
      if (++calls == failure) return false;
      if (!grayscale) {
        EXPECT_TRUE(chrome);
        EXPECT_EQ(r.mode, GfxRenderer::BW);
        r.clearScreen(0x96);
      }
      return true;
    }));
    EXPECT_EQ(r.bw[0], 0x96);
    EXPECT_FALSE(r.absolute);
    EXPECT_EQ(r.events.back(), "display");
  }
}
TEST(ImageViewerGrayscale, RejectedBasesRetryMonoAndCorruptImageReturnsFailure) {
  GfxRenderer r;
  r.rejectBase = true;
  int calls = 0;
  EXPECT_TRUE(renderImageViewerGrayscale(r, [&](bool grayscale, bool) {
    ++calls;
    EXPECT_EQ(grayscale, calls == 1);
    return true;
  }));
  EXPECT_EQ(r.events, (std::vector<std::string>{"direct", "absolute", "cleanup", "display"}));
  EXPECT_FALSE(renderImageViewerGrayscale(r, [](bool, bool) { return false; }));
}
