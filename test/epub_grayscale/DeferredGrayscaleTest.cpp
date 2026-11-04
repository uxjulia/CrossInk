#include <gtest/gtest.h>
class HalDisplay {
 public:
  enum { FAST_REFRESH };
};
#include "DeferredGrayscale.h"
class DeferredGrayscaleTest : public testing::Test {
 protected:
  void SetUp() override { fakeheap::reset(); }
  void TearDown() override { EXPECT_TRUE(fakeheap::live.empty()); }
};
TEST_F(DeferredGrayscaleTest, RendersWhileBaseRunsAndReusesPlanesWithoutChangingBw) {
  GfxRenderer renderer(800, 480);
  ReaderUtils::DeferredGrayscalePlanes planes;
  int cadence = 5;
  const auto bw = renderer.bw;
  auto render = [&] {
    renderer.events.push_back("render");
    renderer.clearScreen(renderer.mode == GfxRenderer::GRAYSCALE_LSB ? 0x35 : 0xA9);
    return true;
  };
  ASSERT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, render));
  EXPECT_EQ(renderer.events, (std::vector<std::string>{"base", "render", "render", "wait", "copy", "gray", "cleanup"}));
  EXPECT_EQ(renderer.bw, bw);
  EXPECT_EQ(renderer.lsb, std::vector<uint8_t>(48000, 0x35));
  EXPECT_EQ(renderer.msb, std::vector<uint8_t>(48000, 0xA9));
  EXPECT_EQ(cadence, 4);
  auto* first = planes.lsb.get();
  const auto allocations = fakeheap::external.attempts;
  ASSERT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, render));
  EXPECT_EQ(planes.lsb.get(), first);
  EXPECT_EQ(fakeheap::external.attempts, allocations);
}
TEST_F(DeferredGrayscaleTest, AllocationFailureLeavesBlockingFallbackUntouched) {
  GfxRenderer renderer;
  ReaderUtils::DeferredGrayscalePlanes planes;
  fakeheap::external.failOnAttempt = 2;
  int cadence = 5;
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_TRUE(renderer.events.empty());
  EXPECT_EQ(cadence, 5);
}
TEST_F(DeferredGrayscaleTest, FailedPlaneStillWaitsAndRestoresBwState) {
  GfxRenderer renderer;
  ReaderUtils::DeferredGrayscalePlanes planes;
  int cadence = 5;
  EXPECT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return false; }));
  EXPECT_EQ(renderer.events, (std::vector<std::string>{"base", "wait", "cleanup"}));
  EXPECT_FALSE(renderer.active);
  EXPECT_EQ(renderer.mode, GfxRenderer::BW);
}
TEST_F(DeferredGrayscaleTest, CleanupTurnsUnsupportedPanelsAndC3DoNotAllocate) {
  GfxRenderer renderer;
  ReaderUtils::DeferredGrayscalePlanes planes;
  int cadence = 1;
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  cadence = 5;
  renderer.deferredSupported = false;
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  renderer.deferredSupported = true;
  fakeheap::reset(false);
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_TRUE(renderer.events.empty());
}
