#include <Epub/Page.h>
#include <EpubGrayscale.h>
#include <gtest/gtest.h>

#include <cstdlib>

namespace {
int failRowAllocations = 0;
std::vector<size_t> rowAllocations;
void* rasterMalloc(size_t n) {
  rowAllocations.push_back(n);
  if (failRowAllocations) {
    --failRowAllocations;
    return nullptr;
  }
  return std::malloc(n);
}
}  // namespace
// Compile the actual cache renderer, including its internal clip ranges, with
// only storage/heap/display adapters. No replacement image raster loop.
#define malloc rasterMalloc
#include "lib/Epub/Epub/blocks/ImageBlock.cpp"
#undef malloc
bool ImageDecoderFactory::supportsMemorySource(const std::string& path) {
  return path.ends_with(".jpg") || path.ends_with(".png");
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }

namespace {
std::vector<uint8_t> cache(int w, int h) {
  std::vector<uint8_t> result(4 + ((w + 3) / 4) * h);
  result[0] = w & 255;
  result[1] = w >> 8;
  result[2] = h & 255;
  result[3] = h >> 8;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) result[4 + y * ((w + 3) / 4) + x / 4] |= ((x + y * 3) % 4) << (6 - (x % 4) * 2);
  return result;
}
// Independent reference: visit every source pixel, rotate coordinates, and
// only then reject physical pixels. This deliberately does not use clip ranges
// or DirectPixelWriter, so a shared clipping error cannot bless candidate bytes.
std::vector<uint8_t> reference(const GfxRenderer& r, int w, int h, int x, int y, int origin, int rows) {
  std::vector<uint8_t> out(size_t(rows) * r.stride, r.mode == GfxRenderer::BW ? 255 : 0);
  for (int iy = 0; iy < h; ++iy)
    for (int ix = 0; ix < w; ++ix) {
      int lx = x + ix, ly = y + iy;
      if (lx < 0 || ly < 0 || lx >= r.getScreenWidth() || ly >= r.getScreenHeight()) continue;
      int px = lx, py = ly;
      switch (r.orientation) {
        case GfxRenderer::Portrait:
          px = ly;
          py = r.height - 1 - lx;
          break;
        case GfxRenderer::PortraitInverted:
          px = r.width - 1 - ly;
          py = lx;
          break;
        case GfxRenderer::LandscapeClockwise:
          px = r.width - 1 - lx;
          py = r.height - 1 - ly;
          break;
        default:
          break;
      }
      if (py < origin || py >= origin + rows) continue;
      int pixel = (ix + iy * 3) % 4;
      bool mark = r.mode == GfxRenderer::BW              ? pixel < 3
                  : r.mode == GfxRenderer::GRAYSCALE_LSB ? pixel == 1
                                                         : pixel == 1 || pixel == 2;
      if (!mark) continue;
      auto& byte = out[size_t(py - origin) * r.stride + px / 8];
      uint8_t mask = 128 >> (px % 8);
      if (r.mode == GfxRenderer::BW)
        byte &= ~mask;
      else
        byte |= mask;
    }
  return out;
}
class EpubGrayscaleTest : public testing::Test {
  void SetUp() override {
    ImageBlock::clearSessionRenderFailures();
    fakeheap::reset(false);
    Storage.reset();
    failRowAllocations = 0;
    rowAllocations.clear();
  }
  void TearDown() override {
    ImageBlock::clearSessionRenderFailures();
    EXPECT_TRUE(fakeheap::live.empty());
  }
};

TEST_F(EpubGrayscaleTest, CachedPixelsMatchReferenceAcrossRotationsModesOffsetsAndStrides) {
  for (bool psram : {false, true})
    for (int width : {792, 800})
      for (int orientation = 0; orientation < 4; ++orientation)
        for (int mode = 0; mode < 3; ++mode) {
          ImageBlock::releaseSessionPixelCache();
          fakeheap::reset(psram);
          GfxRenderer r(width, 481);
          r.orientation = GfxRenderer::Orientation(orientation);
          r.mode = GfxRenderer::RenderMode(mode);
          const int w = r.getScreenWidth() - 3, h = r.getScreenHeight() - 5;
          Storage.put("image.pxc", cache(w, h));
          for (auto [x, y] : {std::pair{0, 0},
                              {-17, -9},
                              {11, 13},
                              {r.getScreenWidth() - 1, 0},
                              {0, r.getScreenHeight() - 1},
                              {-w, 0},
                              {0, -h}}) {
            for (int origin = 0; origin < r.height; origin += 80) {
              SCOPED_TRACE(testing::Message() << psram << ' ' << width << ' ' << orientation << ' ' << mode << ' ' << x
                                              << ' ' << y << ' ' << origin);
              const int rows = std::min(80, r.height - origin);
              std::vector<uint8_t> guarded(size_t(rows) * r.stride + 32, 0xCD);
              std::fill(guarded.begin() + 16, guarded.end() - 16, mode == 0 ? 255 : 0);
              const auto liveBw = r.bw;
              r.beginStripTarget(guarded.data() + 16, origin, rows);
              ASSERT_TRUE(renderFromCache(r, "image.pxc", x, y, w, h));
              EXPECT_EQ(std::vector<uint8_t>(guarded.begin() + 16, guarded.end() - 16),
                        reference(r, w, h, x, y, origin, rows));
              r.endStripTarget();
              EXPECT_EQ(r.bw, liveBw);
              EXPECT_TRUE(std::all_of(guarded.begin(), guarded.begin() + 16, [](auto b) { return b == 0xCD; }));
              EXPECT_TRUE(std::all_of(guarded.end() - 16, guarded.end(), [](auto b) { return b == 0xCD; }));
            }
          }
        }
}

TEST_F(EpubGrayscaleTest, MonochromeCachedImagesKeepMidtoneDetailAcrossRotationsAndCacheHits) {
  constexpr int expectedBlackPixels[] = {256, 176, 80, 0};
  for (bool psram : {false, true}) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      ImageBlock::releaseSessionPixelCache();
      fakeheap::reset(psram);
      GfxRenderer r;
      r.orientation = GfxRenderer::Orientation(orientation);
      for (int level = 0; level < 4; ++level) {
        auto pixels = cache(16, 16);
        std::fill(pixels.begin() + 4, pixels.end(), level * 0x55);
        const std::string path = "level" + std::to_string(level) + ".pxc";
        Storage.put(path, pixels);
        for (int pass = 0; pass < 2; ++pass) {
          r.clearScreen(255);
          ASSERT_TRUE(renderFromCache(r, path, 9, 13, 16, 16, false));
          int black = 0;
          for (auto byte : r.bw) {
            for (int bit = 0; bit < 8; ++bit) black += (byte & (1 << bit)) == 0;
          }
          EXPECT_EQ(black, expectedBlackPixels[level]);
        }
        // Turning grayscale back on must use the existing dark base and cache.
        r.clearScreen(255);
        ASSERT_TRUE(renderFromCache(r, path, 9, 13, 16, 16, true));
        int black = 0;
        for (auto byte : r.bw) {
          for (int bit = 0; bit < 8; ++bit) black += (byte & (1 << bit)) == 0;
        }
        EXPECT_EQ(black, level < 3 ? 256 : 0);
      }
    }
  }
}

TEST_F(EpubGrayscaleTest, MonochromeDitherPreservesSourceShadesAfterFourLevelQuantization) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    GfxRenderer r;
    r.orientation = GfxRenderer::Orientation(orientation);
    int previousBlack = 257;
    for (int gray : {0, 32, 64, 96, 128, 160, 192, 224, 255}) {
      auto pixels = cache(16, 16);
      std::fill(pixels.begin() + 4, pixels.end(), 0);
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
          const auto level = applyBayerDither4Level(gray, x + 9, y + 13);
          pixels[4 + y * 4 + x / 4] |= level << (6 - (x & 3) * 2);
        }
      }
      Storage.put("source.pxc", pixels);
      r.clearScreen(255);
      ASSERT_TRUE(renderFromCache(r, "source.pxc", 9, 13, 16, 16, false));
      int black = 0;
      for (auto byte : r.bw) {
        for (int bit = 0; bit < 8; ++bit) black += (byte & (1 << bit)) == 0;
      }
      // Increasing source brightness must retain distinct tones, including
      // quarter tones; the two ordered dither stages must not cancel out.
      EXPECT_LT(black, previousBlack) << gray;
      if (gray == 0) EXPECT_EQ(black, 256);
      if (gray == 255) EXPECT_EQ(black, 0);
      if (gray == 128) EXPECT_EQ(black, 128);
      previousBlack = black;
    }
  }
}

TEST_F(EpubGrayscaleTest, NoStripUsesFullScreenAndRetainedHitAvoidsReads) {
  fakeheap::reset(true);
  GfxRenderer r;
  const int w = 479, h = 789;
  Storage.put("image.pxc", cache(w, h));
  for (int mode = 0; mode < 3; ++mode) {
    r.mode = GfxRenderer::RenderMode(mode);
    r.clearScreen(mode == 0 ? 255 : 0);
    ASSERT_TRUE(renderFromCache(r, "image.pxc", -3, -5, w, h));
    EXPECT_EQ(r.bw, reference(r, w, h, -3, -5, 0, r.height));
    EXPECT_EQ(Storage.data("image.pxc").readCalls, 3u);
  }
}

TEST_F(EpubGrayscaleTest, PortraitClipsCandidatesButKeepsBatchedPayloadReads) {
  GfxRenderer r(800, 480);
  r.orientation = GfxRenderer::PortraitInverted;
  const int w = 479, h = 797;
  Storage.put("image.pxc", cache(w, h));
  std::vector<uint8_t> scratch(r.stride * 80);
  r.beginStripTarget(scratch.data(), 80, 80);
  const auto clip = cachedImageClip(r, 0, 0, w, h);
  EXPECT_EQ((clip.x1 - clip.x0) * (clip.y1 - clip.y0), 80 * h);
  EXPECT_LT((clip.x1 - clip.x0) * (clip.y1 - clip.y0), w * h);
  ASSERT_TRUE(renderFromCache(r, "image.pxc", 0, 0, w, h));
  EXPECT_EQ(Storage.data("image.pxc").readBytes, cache(w, h).size());
  EXPECT_LT(Storage.data("image.pxc").readCalls, 30u);
  EXPECT_EQ(rowAllocations, std::vector<size_t>{4080});
  r.endStripTarget();
}

TEST_F(EpubGrayscaleTest, LandscapeOnlyReadsIntersectingRows) {
  GfxRenderer r(800, 480);
  r.orientation = GfxRenderer::LandscapeCounterClockwise;
  Storage.put("image.pxc", cache(797, 479));
  std::vector<uint8_t> scratch(r.stride * 80);
  r.beginStripTarget(scratch.data(), 80, 80);
  ASSERT_TRUE(renderFromCache(r, "image.pxc", 0, 0, 797, 479));
  EXPECT_EQ(Storage.data("image.pxc").readBytes, 4u + 200 * 80);
  r.endStripTarget();
}

TEST_F(EpubGrayscaleTest, EmptyClipDoesNotAllocateOrReadPayload) {
  fakeheap::reset(true);
  GfxRenderer r;
  Storage.put("image.pxc", cache(21, 23));
  std::vector<uint8_t> scratch(r.stride * 80);
  r.beginStripTarget(scratch.data(), 0, 80);
  ASSERT_TRUE(renderFromCache(r, "image.pxc", 0, 0, 21, 23));
  EXPECT_EQ(Storage.data("image.pxc").readBytes, 4u);
  EXPECT_EQ(fakeheap::external.attempts, 0u);
  EXPECT_TRUE(rowAllocations.empty());
  r.endStripTarget();
}

TEST_F(EpubGrayscaleTest, OnePixelAndExactStripEdges) {
  GfxRenderer r(792, 481);
  r.orientation = GfxRenderer::PortraitInverted;
  std::vector<uint8_t> scratch(r.stride * 80);
  r.beginStripTarget(scratch.data(), 80, 80);
  EXPECT_TRUE(cachedImageClip(r, 59, 0, 21, 23).empty());
  auto clip = cachedImageClip(r, 60, 0, 21, 23);
  EXPECT_EQ(clip.x1 - clip.x0, 1);
  EXPECT_TRUE(cachedImageClip(r, 160, 0, 21, 23).empty());
  clip = cachedImageClip(r, 159, 0, 21, 23);
  EXPECT_EQ(clip.x1 - clip.x0, 1);
  r.endStripTarget();
}

TEST_F(EpubGrayscaleTest, RowAllocationFallsBackAndFailuresCloseCache) {
  GfxRenderer r;
  Storage.put("image.pxc", cache(479, 789));
  failRowAllocations = 1;
  ASSERT_TRUE(renderFromCache(r, "image.pxc", 0, 0, 479, 789));
  EXPECT_EQ(rowAllocations, (std::vector<size_t>{4080, 120}));
  failRowAllocations = 2;
  EXPECT_FALSE(renderFromCache(r, "image.pxc", 0, 0, 479, 789));
  EXPECT_EQ(Storage.openHandles("image.pxc"), 0);
  Storage.data("image.pxc").readFailAt = 10;
  EXPECT_FALSE(renderFromCache(r, "image.pxc", 0, 0, 479, 789));
  EXPECT_EQ(Storage.openHandles("image.pxc"), 0);
  Storage.put("image.pxc", {1, 2, 3});
  EXPECT_FALSE(renderFromCache(r, "image.pxc", 0, 0, 479, 789));
}

TEST_F(EpubGrayscaleTest, RetentionFailureFallsBackWithoutOverrunningOldBuffer) {
  fakeheap::reset(true);
  GfxRenderer r;
  Storage.put("small.pxc", cache(11, 13));
  ASSERT_TRUE(renderFromCache(r, "small.pxc", 0, 0, 11, 13));
  Storage.put("large.pxc", cache(479, 789));
  fakeheap::external.fail = 1;
  r.clearScreen(255);
  ASSERT_TRUE(renderFromCache(r, "large.pxc", 0, 0, 479, 789));
  EXPECT_EQ(r.bw, reference(r, 479, 789, 0, 0, 0, r.height));
  EXPECT_EQ(retainedPxcCapacity, 39u);
}

TEST_F(EpubGrayscaleTest, ImageBlockPreservesScanAndFailureFallback) {
  GfxRenderer r;
  ImageBlock block("image.jpg", "", 21, 23);
  r.fonts.scanning = true;
  block.render(r, 0, 0, true);
  EXPECT_EQ(r.placeholders, 0);
  r.fonts.scanning = false;
  Storage.put("image_v2.pxc", {0, 0, 0});
  block.render(r, 0, 0, true);
  EXPECT_GT(r.placeholders, 0);
  EXPECT_FALSE(r.active);
  EXPECT_EQ(r.mode, GfxRenderer::BW);
}

TEST_F(EpubGrayscaleTest, PlanePathsProduceIdenticalBytesAndRestoreBw) {
  for (int orientation = 0; orientation < 4; ++orientation)
    for (bool text : {false, true}) {
      std::vector<uint8_t> expectedLsb, expectedMsb;
      for (int path = 0; path < 5; ++path) {
        ImageBlock::releaseSessionPixelCache();
        fakeheap::reset(path != 0);
        GfxRenderer r;
        r.orientation = GfxRenderer::Orientation(orientation);
        // Strips, async allocation failures, two async planes, retained full scratch.
        if (path == 1) fakeheap::external.failOnAttempt = 1;
        if (path == 2) fakeheap::external.failOnAttempt = 2;
        int w = r.getScreenWidth() - 7, h = r.getScreenHeight() - 11;
        Storage.put("one_v2.pxc", cache(w, h));
        Storage.put("two_v2.pxc", cache(21, 23));
        ImageBlock one("one.jpg", "", w, h), two("two.jpg", "", 21, 23);
        Page page;
        page.images = {{&one, -3, 2}, {&two, 9, 83}};
        std::vector<uint8_t> scratch(r.stride * (path == 4 ? r.height : 80));
        const auto live = r.bw;
        ASSERT_TRUE(EpubGrayscale::runTiledGrayscalePass(r, page, 1, 0, 0, true, text, true, scratch.data(),
                                                         scratch.size(), path != 0 && path != 4));
        EXPECT_EQ(page.imageVisits, path < 2 ? 14 : 2);
        EXPECT_EQ(page.allVisits, text ? page.imageVisits : 0);
        EXPECT_EQ(r.bw, live);
        EXPECT_EQ(r.mode, GfxRenderer::BW);
        EXPECT_FALSE(r.active);
        if (path == 0) {
          expectedLsb = r.lsb;
          expectedMsb = r.msb;
        } else {
          EXPECT_EQ(r.lsb, expectedLsb);
          EXPECT_EQ(r.msb, expectedMsb);
          if (path != 4) EXPECT_EQ(r.events.front(), "wait");
        }
        std::vector<std::string> expected;
        if (path != 0 && path != 4) expected.push_back("wait");
        expected.insert(expected.end(), path < 2 ? 7 : 1, "lsb");
        expected.insert(expected.end(), path < 2 ? 7 : 1, "msb");
        expected.push_back("gray");
        expected.push_back("cleanup");
        EXPECT_EQ(r.events, expected);
      }
    }
}

TEST_F(EpubGrayscaleTest, ScratchAllocationKeepsFullPlanesInPsramAndFallsBackToSmallStrips) {
  constexpr size_t planeBytes = 96 * 552;
  constexpr size_t stripBytes = 96 * 80;
  for (int scenario = 0; scenario < 7; ++scenario) {
    SCOPED_TRACE(scenario);
    fakeheap::reset(scenario != 1);
    if (scenario == 2) fakeheap::external.free = planeBytes + 128 * 1024 - 1;
    if (scenario == 3) fakeheap::external.largest = stripBytes;
    if (scenario >= 4) fakeheap::external.fail = scenario == 4 ? 1 : 2;
    if (scenario == 6) fakeheap::internal.fail = 1;
    size_t size = 123;
    bool inPsram = true;
    auto scratch = EpubGrayscale::allocateScratch(96, 552, size, inPsram);
    if (scenario == 6) {
      EXPECT_FALSE(scratch);
      EXPECT_EQ(size, 0u);
      EXPECT_FALSE(inPsram);
    } else {
      ASSERT_TRUE(scratch);
      EXPECT_EQ(size, scenario == 0 ? planeBytes : stripBytes);
      EXPECT_EQ(inPsram, scenario != 1 && scenario != 5);
      EXPECT_EQ(fakeheap::live.at(scratch.get()).bytes, size);
      EXPECT_EQ(fakeheap::live.at(scratch.get()).external, inPsram);
    }
    EXPECT_EQ(fakeheap::internal.attempts, scenario == 1 || scenario >= 5 ? 1u : 0u);
  }
}

TEST_F(EpubGrayscaleTest, RetainedEegoPlaneReusesStorageAndPreservesPixelsWithoutAllocations) {
  fakeheap::reset(true);
  size_t size = 0;
  bool inPsram = false;
  auto scratch = EpubGrayscale::allocateScratch(96, 552, size, inPsram);
  ASSERT_TRUE(scratch);
  ASSERT_EQ(size, 52992u);
  const auto attempts = fakeheap::external.attempts;
  for (int turn = 0; turn < 3; ++turn) {
    GfxRenderer r(768, 552);
    Page page;
    const auto live = r.bw;
    ASSERT_TRUE(EpubGrayscale::runTiledGrayscalePass(r, page, 1, 0, 0, true, true, false, scratch.get(), size, false));
    EXPECT_EQ(page.allVisits, 2);
    EXPECT_EQ(r.bw, live);
    EXPECT_EQ(r.events, (std::vector<std::string>{"lsb", "msb", "gray", "cleanup"}));
    EXPECT_EQ(fakeheap::external.attempts, attempts);
    EXPECT_EQ(fakeheap::internal.attempts, 0u);
  }
}

TEST_F(EpubGrayscaleTest, DisabledImageGrayscaleNeverVisitsImagesWithTextAaOrWithoutIt) {
  for (bool text : {false, true}) {
    for (int path = 0; path < 4; ++path) {
      fakeheap::reset(path != 0);
      if (path == 1) fakeheap::external.failOnAttempt = 1;
      if (path == 2) fakeheap::external.failOnAttempt = 2;
      GfxRenderer r;
      ImageBlock image("disabled.jpg", "", 21, 23);
      Page page;
      page.images = {{&image, 0, 0}};
      const auto live = r.bw;
      std::vector<uint8_t> scratch(r.stride * 80);
      EXPECT_EQ(EpubGrayscale::runTiledGrayscalePass(r, page, 1, 0, 0, true, text, false, scratch.data(),
                                                     scratch.size(), path != 0),
                text);
      EXPECT_EQ(page.imageVisits, 0);
      EXPECT_EQ(page.allVisits, text ? (path < 2 ? 14 : 2) : 0);
      EXPECT_EQ(r.bw, live);
      EXPECT_EQ(r.mode, GfxRenderer::BW);
      EXPECT_FALSE(r.active);
      if (text) {
        EXPECT_TRUE(std::all_of(r.lsb.begin(), r.lsb.end(), [](auto b) { return b == 0; }));
        EXPECT_TRUE(std::all_of(r.msb.begin(), r.msb.end(), [](auto b) { return b == 0; }));
      } else {
        EXPECT_TRUE(r.events.empty());
      }
    }
  }
}

TEST_F(EpubGrayscaleTest, MissingScratchAndUnsupportedPathsPreserveFallbackContract) {
  for (int reason = 0; reason < 5; ++reason) {
    GfxRenderer r;
    Page page;
    const auto live = r.bw;
    fakeheap::internal.free = 0;
    if (reason == 0) r.supported = false;
    if (reason == 1) r.inverted = true;
    EXPECT_FALSE(EpubGrayscale::runTiledGrayscalePass(r, page, 1, 0, 0, true, reason != 2, reason != 2, nullptr, 0,
                                                      reason != 4));
    EXPECT_EQ(r.bw, live);
    EXPECT_EQ(r.mode, GfxRenderer::BW);
    EXPECT_FALSE(r.active);
    EXPECT_EQ(page.imageVisits, 0);
    EXPECT_EQ(r.events, reason == 3 ? (std::vector<std::string>{"wait", "cleanup"}) : std::vector<std::string>{});
  }
}
TEST_F(EpubGrayscaleTest, AbsoluteImagesReplaceAllFourLevelsAndPreserveTheRestOfThePage) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    ImageBlock::releaseSessionPixelCache();
    GfxRenderer r;
    r.orientation = GfxRenderer::Orientation(orientation);
    r.absolute = true;
    const auto base = r.bw;  // Represents text, bars and feedback outside the image.
    Storage.put("absolute_v2.pxc", cache(21, 23));
    ImageBlock image("absolute.jpg", "", 21, 23);
    Page page;
    page.images = {{&image, 9, 13}};
    std::vector<uint8_t> low(base.size()), high(base.size());
    ASSERT_TRUE(EpubGrayscale::runAbsoluteImageGrayscalePass(r, page, 1, 0, 0, low.data(), high.data(), low.size()));
    for (int plane = 0; plane < 2; ++plane) {
      auto expected = base;
      for (int y = 0; y < 23; ++y) {
        for (int x = 0; x < 21; ++x) {
          const int lx = x + 9, ly = y + 13;
          int px = lx, py = ly;
          if (orientation == GfxRenderer::Portrait) {
            px = ly;
            py = r.height - 1 - lx;
          }
          if (orientation == GfxRenderer::PortraitInverted) {
            px = r.width - 1 - ly;
            py = lx;
          }
          if (orientation == GfxRenderer::LandscapeClockwise) {
            px = r.width - 1 - lx;
            py = r.height - 1 - ly;
          }
          const int level = (x + y * 3) % 4;
          const uint8_t mask = 0x80 >> (px & 7);
          auto& byte = expected[size_t(py) * r.stride + px / 8];
          if ((level >> plane) & 1)
            byte |= mask;
          else
            byte &= ~mask;
        }
      }
      EXPECT_EQ(plane ? r.msb : r.lsb, expected);
    }
    EXPECT_EQ(r.bw, base);
    EXPECT_FALSE(r.absolute);
    EXPECT_EQ(r.mode, GfxRenderer::BW);
    EXPECT_EQ(r.events, (std::vector<std::string>{"copy", "gray", "cleanup"}));
    EXPECT_EQ(page.allVisits, 0);
    EXPECT_EQ(page.imageVisits, 2);
  }
}

TEST_F(EpubGrayscaleTest, IncompleteAbsolutePlaneBuffersDoNotModifyThePageOrStartUploads) {
  GfxRenderer r;
  Page page;
  const auto base = r.bw;
  std::vector<uint8_t> plane(base.size());
  for (int failure = 0; failure < 4; ++failure) {
    r.absolute = failure != 0;
    EXPECT_FALSE(EpubGrayscale::runAbsoluteImageGrayscalePass(r, page, 1, 0, 0, failure == 1 ? nullptr : plane.data(),
                                                              failure == 2 ? nullptr : plane.data(),
                                                              failure == 3 ? plane.size() - 1 : plane.size()));
    EXPECT_EQ(r.bw, base);
    EXPECT_TRUE(r.events.empty());
    EXPECT_EQ(page.imageVisits, 0);
  }
}

TEST_F(EpubGrayscaleTest, AbsoluteFeedbackIsDrawnAfterImagesInBothPlanes) {
  GfxRenderer r(32, 32);
  r.orientation = GfxRenderer::LandscapeCounterClockwise;
  r.absolute = true;
  const auto base = r.bw;
  Storage.put("feedback_v2.pxc", cache(21, 23));
  ImageBlock image("feedback.jpg", "", 21, 23);
  Page page;
  page.images = {{&image, 0, 0}};
  struct Overlay {
    GfxRenderer* renderer;
    int calls = 0;
  } overlay{&r};
  std::vector<uint8_t> low(base.size()), high(base.size());
  ASSERT_TRUE(EpubGrayscale::runAbsoluteImageGrayscalePass(
      r, page, 1, 0, 0, low.data(), high.data(), low.size(),
      [](void* context) {
        auto& overlay = *static_cast<Overlay*>(context);
        EXPECT_TRUE(overlay.renderer->isStripTargetActive());
        // A B/W feedback pattern covering the image's first eight pixels.
        overlay.renderer->getWriteTarget()[0] = 0x96;
        ++overlay.calls;
      },
      &overlay));
  EXPECT_EQ(overlay.calls, 2);
  EXPECT_EQ(r.lsb[0], 0x96);
  EXPECT_EQ(r.msb[0], 0x96);
  EXPECT_EQ(r.bw, base);
  EXPECT_EQ(r.events, (std::vector<std::string>{"copy", "gray", "cleanup"}));
}
}  // namespace
