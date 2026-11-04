#include <gtest/gtest.h>

#include "lib/Epub/Epub/blocks/ImageBlock.cpp"

bool ImageDecoderFactory::supportsMemorySource(const std::string& path) {
  return path.ends_with(".jpg") || path.ends_with(".png");
}

static ImageToFramebufferDecoder* testDecoder = nullptr;
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return testDecoder; }

namespace {

std::vector<uint8_t> cache(const int width, const int height, const int seed = 0) {
  std::vector<uint8_t> result(4 + static_cast<size_t>((width + 3) / 4) * height);
  result[0] = width & 255;
  result[1] = width >> 8;
  result[2] = height & 255;
  result[3] = height >> 8;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      result[4 + static_cast<size_t>(y) * ((width + 3) / 4) + x / 4] |= ((x + y * 3 + seed) % 4) << (6 - (x % 4) * 2);
  return result;
}

struct SeedContext {
  int calls = 0;
  std::vector<uint8_t> bytes;
};

bool seedCache(void* context, const char*, const int, const int, const char* destination) {
  auto& seed = *static_cast<SeedContext*>(context);
  ++seed.calls;
  Storage.put(destination, seed.bytes);
  return true;
}

class RetainedPxcCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    ImageBlock::clearSessionRenderFailures();
    ImageBlock::setExtractor(nullptr, nullptr, nullptr);
    testDecoder = nullptr;
    fakeheap::reset(true);
    Storage.reset();
  }

  void TearDown() override {
    ImageBlock::setExtractor(nullptr, nullptr, nullptr);
    ImageBlock::clearSessionRenderFailures();
    EXPECT_TRUE(fakeheap::live.empty());
  }

  static void render(GfxRenderer& renderer, const std::string& path, const int width, const int height) {
    renderer.clearScreen(255);
    ASSERT_TRUE(renderFromCache(renderer, path, 0, 0, width, height));
  }
};

TEST_F(RetainedPxcCacheTest, AlternatingImagesAvoidPayloadReadsAndKeepPixelsAcrossOrientation) {
  constexpr int width = 200;
  constexpr int height = 120;
  Storage.put("a.pxc", cache(width, height, 0));
  Storage.put("b.pxc", cache(width, height, 1));
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;

  render(renderer, "a.pxc", width, height);
  const auto firstA = renderer.bw;
  render(renderer, "b.pxc", width, height);
  const auto firstB = renderer.bw;
  const size_t aBytesBeforeHit = Storage.data("a.pxc").readBytes;
  const size_t bBytesBeforeHit = Storage.data("b.pxc").readBytes;

  render(renderer, "a.pxc", width, height);
  EXPECT_EQ(renderer.bw, firstA);
  EXPECT_EQ(Storage.data("a.pxc").readBytes - aBytesBeforeHit, 0u);
  renderer.orientation = GfxRenderer::Portrait;
  render(renderer, "b.pxc", width, height);
  EXPECT_EQ(Storage.data("b.pxc").readBytes - bBytesBeforeHit, 0u);
  EXPECT_EQ(retainedPxcCapacity, 2u * static_cast<size_t>((width + 3) / 4) * height);
}

TEST_F(RetainedPxcCacheTest, ThirdImageEvictsLruWithoutExceedingAggregateBudget) {
  constexpr int width = 800;
  constexpr int height = 320;
  constexpr size_t payloadBytes = static_cast<size_t>(width / 4) * height;
  Storage.put("a.pxc", cache(width, height, 0));
  Storage.put("b.pxc", cache(width, height, 1));
  Storage.put("c.pxc", cache(width, height, 2));
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;

  render(renderer, "a.pxc", width, height);
  render(renderer, "b.pxc", width, height);
  render(renderer, "a.pxc", width, height);  // b is the LRU entry.
  ASSERT_EQ(retainedPxcCapacity, 2u * payloadBytes);
  ASSERT_EQ(fakeheap::external.attempts, 2u);

  render(renderer, "c.pxc", width, height);
  EXPECT_EQ(retainedPxcCapacity, 2u * payloadBytes);
  EXPECT_LE(retainedPxcCapacity, MAX_RETAINED_PXC_BYTES);
  EXPECT_EQ(fakeheap::external.attempts, 2u);

  const size_t aBytesBeforeHit = Storage.data("a.pxc").readBytes;
  const size_t bBytesBeforeMiss = Storage.data("b.pxc").readBytes;
  render(renderer, "a.pxc", width, height);
  render(renderer, "b.pxc", width, height);
  EXPECT_EQ(Storage.data("a.pxc").readBytes - aBytesBeforeHit, 0u);
  EXPECT_EQ(Storage.data("b.pxc").readBytes - bBytesBeforeMiss, payloadBytes + 4u);
}

TEST_F(RetainedPxcCacheTest, LargerImageWithinRemainingBudgetEvictsOnlyLru) {
  Storage.put("a.pxc", cache(800, 160, 0));  // 32,000 bytes
  Storage.put("b.pxc", cache(800, 160, 1));
  Storage.put("c.pxc", cache(800, 200, 2));  // 40,000 bytes
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;

  render(renderer, "a.pxc", 800, 160);
  render(renderer, "b.pxc", 800, 160);
  render(renderer, "a.pxc", 800, 160);  // b is the LRU entry.

  render(renderer, "c.pxc", 800, 200);
  EXPECT_EQ(retainedPxcCapacity, static_cast<size_t>(800 / 4) * (160 + 200));
  EXPECT_EQ(fakeheap::external.attempts, 3u);

  const size_t aBytesBeforeHit = Storage.data("a.pxc").readBytes;
  render(renderer, "a.pxc", 800, 160);
  EXPECT_EQ(Storage.data("a.pxc").readBytes - aBytesBeforeHit, 0u);
}

TEST_F(RetainedPxcCacheTest, LargerImageEvictsBothEntriesBeforeItsAllocation) {
  Storage.put("small.pxc", cache(800, 160, 0));   // 32,000 bytes
  Storage.put("medium.pxc", cache(800, 320, 1));  // 64,000 bytes
  Storage.put("large.pxc", cache(800, 480, 2));   // 96,000 bytes
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;

  render(renderer, "small.pxc", 800, 160);
  render(renderer, "medium.pxc", 800, 320);
  ASSERT_EQ(retainedPxcCapacity, static_cast<size_t>(800 / 4) * (160 + 320));

  render(renderer, "large.pxc", 800, 480);
  EXPECT_EQ(retainedPxcCapacity, static_cast<size_t>(800 / 4) * 480);
  EXPECT_LE(retainedPxcCapacity, MAX_RETAINED_PXC_BYTES);
  EXPECT_EQ(fakeheap::external.attempts, 3u);
}

TEST_F(RetainedPxcCacheTest, AllocationFailureStreamsWithoutDiscardingTheOtherRetainedImage) {
  constexpr int width = 200;
  constexpr int height = 120;
  constexpr size_t payloadBytes = static_cast<size_t>((width + 3) / 4) * height;
  Storage.put("a.pxc", cache(width, height, 0));
  Storage.put("b.pxc", cache(width, height, 1));
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;

  render(renderer, "a.pxc", width, height);
  const auto expected = renderer.bw;
  fakeheap::external.fail = 1;
  render(renderer, "b.pxc", width, height);
  EXPECT_EQ(retainedPxcCapacity, payloadBytes);

  const size_t aBytesBeforeHit = Storage.data("a.pxc").readBytes;
  render(renderer, "a.pxc", width, height);
  EXPECT_EQ(renderer.bw, expected);
  EXPECT_EQ(Storage.data("a.pxc").readBytes - aBytesBeforeHit, 0u);
}

TEST_F(RetainedPxcCacheTest, RetentionLeavesPsramForTheNextJpegDecoder) {
  constexpr int width = 200;
  constexpr int height = 120;
  constexpr size_t payloadBytes = static_cast<size_t>((width + 3) / 4) * height;
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM;
  fakeheap::internal.largest = 0;

  const size_t withoutJpeg = payloadBytes + MemoryBudget::EPUB_PSRAM_RESERVE;
  fakeheap::external = {withoutJpeg, withoutJpeg, withoutJpeg};
  Storage.put("image.pxc", cache(width, height));
  render(renderer, "image.pxc", width, height);
  EXPECT_EQ(retainedPxcCapacity, 0u);
  EXPECT_EQ(fakeheap::external.attempts, 0u);

  Storage.reset();
  fakeheap::reset(true);
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM;
  fakeheap::internal.largest = 0;
  const size_t withJpeg = withoutJpeg + MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::external = {withJpeg, withJpeg, withJpeg};
  Storage.put("image.pxc", cache(width, height));
  render(renderer, "image.pxc", width, height);
  EXPECT_EQ(retainedPxcCapacity, payloadBytes);
  EXPECT_EQ(fakeheap::external.attempts, 1u);
  EXPECT_TRUE(MemoryBudget::hasHeapForJpegDecoder("TEST", MemoryBudget::JPEG_DECODER_APPROX_BYTES, "image.jpg"));
}

TEST_F(RetainedPxcCacheTest, RegeneratingTheSamePathInvalidatesRetainedPixels) {
  constexpr int width = 200;
  constexpr int height = 120;
  Storage.put("image.pxc", cache(width, height, 0));
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;
  render(renderer, "image.pxc", width, height);
  const auto stalePixels = renderer.bw;

  Storage.remove("image.pxc");
  SeedContext seed{.bytes = cache(width, height, 1)};
  ImageBlock::setExtractor(&seed, nullptr, seedCache);
  ImageBlock image("image.jpg", "source.jpg", width, height);
  image.prepareCache();
  ASSERT_EQ(seed.calls, 1);

  const size_t bytesBeforeReload = Storage.data("image.pxc").readBytes;
  render(renderer, "image.pxc", width, height);
  EXPECT_NE(renderer.bw, stalePixels);
  EXPECT_EQ(Storage.data("image.pxc").readBytes - bytesBeforeReload,
            4u + static_cast<size_t>((width + 3) / 4) * height);
}

TEST_F(RetainedPxcCacheTest, ChangedDimensionsBypassThePreviousPathEntry) {
  Storage.put("image.pxc", cache(200, 120, 0));
  GfxRenderer renderer(800, 480);
  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;
  render(renderer, "image.pxc", 200, 120);

  Storage.put("image.pxc", cache(200, 122, 1));
  const size_t bytesBeforeReload = Storage.data("image.pxc").readBytes;
  render(renderer, "image.pxc", 200, 122);
  EXPECT_EQ(Storage.data("image.pxc").readBytes - bytesBeforeReload, 4u + static_cast<size_t>(200 / 4) * 122);
}

}  // namespace

namespace {
struct SourceDecoder : ImageToFramebufferDecoder {
  bool memory = false;
  int calls = 0;
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig& config) override {
    ++calls;
    memory = config.sourceData != nullptr;
    if (memory) {
      EXPECT_EQ(config.sourceSize, 8u);
      EXPECT_EQ(config.sourceData[0], 42);
    }
    return true;
  }
  bool getDimensions(const std::string&, ImageDimensions&) const override { return false; }
  const char* getFormatName() const override { return "test"; }
};
struct SourceContext {
  int loaded = 0, extracted = 0;
  bool fail = false;
};
bool extractSource(void* context, const char*, const char* path) {
  ++static_cast<SourceContext*>(context)->extracted;
  Storage.put(path, std::vector<uint8_t>(8, 42));
  return true;
}
HeapByteBuffer loadSource(void* context, const char*, size_t& size) {
  auto& state = *static_cast<SourceContext*>(context);
  ++state.loaded;
  if (state.fail) return {};
  auto source = makePsramByteBufferNoThrow(8);
  if (source) {
    memset(source.get(), 42, 8);
    size = 8;
  }
  return source;
}
}  // namespace
TEST_F(RetainedPxcCacheTest, MemoryImageSourceAvoidsTemporarySdFile) {
  SourceContext context;
  SourceDecoder decoder;
  testDecoder = &decoder;
  ImageBlock::setExtractor(&context, extractSource, nullptr, loadSource);
  ImageBlock block("source.jpg", "chapter/image.jpg", 20, 20);
  block.prepareCache();
  EXPECT_EQ(context.extracted, 0);
  GfxRenderer renderer;
  block.render(renderer, 0, 0, true);
  EXPECT_EQ(context.loaded, 1);
  EXPECT_EQ(context.extracted, 0);
  EXPECT_TRUE(decoder.memory);
  EXPECT_FALSE(Storage.exists("source.jpg"));
  EXPECT_TRUE(fakeheap::live.empty());
}
TEST_F(RetainedPxcCacheTest, MemoryImageFailureFallsBackToSdExtraction) {
  SourceContext context;
  context.fail = true;
  SourceDecoder decoder;
  testDecoder = &decoder;
  ImageBlock::setExtractor(&context, extractSource, nullptr, loadSource);
  ImageBlock block("source.png", "chapter/image.png", 20, 20);
  block.prepareCache();
  GfxRenderer renderer;
  block.render(renderer, 0, 0, true);
  EXPECT_EQ(context.loaded, 1);
  EXPECT_EQ(context.extracted, 1);
  EXPECT_FALSE(decoder.memory);
  EXPECT_EQ(decoder.calls, 1);
}
