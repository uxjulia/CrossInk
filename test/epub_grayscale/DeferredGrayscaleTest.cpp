#include <gtest/gtest.h>
class HalDisplay {
 public:
  enum { FAST_REFRESH };
};
#include "DeferredGrayscale.h"
#include "ReaderGrayscale.h"
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

TEST_F(DeferredGrayscaleTest, BlockingAaBackupFailureCompletesPendingBwPage) {
  GfxRenderer renderer;
  renderer.failBwBackup = true;
  const auto bw = renderer.bw;
  int renderCalls = 0;
  ReaderUtils::renderAntiAliased(renderer, [&] { ++renderCalls; });
  EXPECT_EQ(renderCalls, 0);
  EXPECT_EQ(renderer.events, (std::vector<std::string>{"cleanup"}));
  EXPECT_EQ(renderer.bw, bw);
  EXPECT_EQ(renderer.mode, GfxRenderer::BW);
}

TEST_F(DeferredGrayscaleTest, BlockingAaUploadsBothMasksAndRestoresBwBeforeCleanup) {
  GfxRenderer renderer;
  const auto bw = renderer.bw;
  ReaderUtils::renderAntiAliased(
      renderer, [&] { renderer.clearScreen(renderer.mode == GfxRenderer::GRAYSCALE_LSB ? 0x35 : 0xA9); });
  EXPECT_EQ(renderer.events, (std::vector<std::string>{"gray", "cleanup"}));
  EXPECT_EQ(renderer.lsb, std::vector<uint8_t>(renderer.bw.size(), 0x35));
  EXPECT_EQ(renderer.msb, std::vector<uint8_t>(renderer.bw.size(), 0xA9));
  EXPECT_EQ(renderer.bw, bw);
  EXPECT_EQ(renderer.mode, GfxRenderer::BW);
}

TEST_F(DeferredGrayscaleTest, CombinedPageRendersEachPlaneOnceAndReusesBuffers) {
  GfxRenderer renderer(800, 480);
  renderer.deferredSupported = false;
  renderer.combinedBase = true;
  ReaderUtils::DeferredGrayscalePlanes planes;
  const auto bw = renderer.bw;
  int cadence = 5;
  auto render = [&] {
    EXPECT_TRUE(renderer.active);
    EXPECT_EQ(renderer.origin, 0);
    EXPECT_EQ(renderer.rows, 480);
    renderer.events.push_back("render");
    renderer.clearScreen(renderer.mode == GfxRenderer::GRAYSCALE_LSB ? 0x35 : 0xA9);
    return true;
  };
  ASSERT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, render));
  EXPECT_EQ(renderer.events,
            (std::vector<std::string>{"combined-base", "render", "render", "copy", "gray", "cleanup"}));
  EXPECT_EQ(renderer.lsb, std::vector<uint8_t>(48000, 0x35));
  EXPECT_EQ(renderer.msb, std::vector<uint8_t>(48000, 0xA9));
  EXPECT_EQ(renderer.bw, bw);
  EXPECT_EQ(renderer.mode, GfxRenderer::BW);
  EXPECT_EQ(cadence, 4);
  auto* first = planes.lsb.get();
  const auto allocations = fakeheap::external.attempts;
  ASSERT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, render));
  EXPECT_EQ(planes.lsb.get(), first);
  EXPECT_EQ(fakeheap::external.attempts, allocations);
}

TEST_F(DeferredGrayscaleTest, CombinedAllocationFailureLeavesStripFallbackUntouched) {
  GfxRenderer renderer;
  renderer.deferredSupported = false;
  renderer.combinedBase = true;
  ReaderUtils::DeferredGrayscalePlanes planes;
  fakeheap::external.failOnAttempt = 2;
  int cadence = 5;
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_TRUE(renderer.events.empty());
  EXPECT_EQ(cadence, 5);
}

TEST_F(DeferredGrayscaleTest, CombinedFailedPlaneCompletesBwFallbackWithoutUploadingPartialAa) {
  GfxRenderer renderer;
  renderer.deferredSupported = false;
  renderer.combinedBase = true;
  ReaderUtils::DeferredGrayscalePlanes planes;
  const auto bw = renderer.bw;
  int cadence = 5;
  EXPECT_TRUE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return false; }));
  EXPECT_EQ(renderer.events, (std::vector<std::string>{"combined-base", "cleanup"}));
  EXPECT_EQ(renderer.bw, bw);
  EXPECT_FALSE(renderer.active);
  EXPECT_EQ(renderer.mode, GfxRenderer::BW);
}

TEST_F(DeferredGrayscaleTest, CombinedCleanupTurnsAndMissingPsramKeepExistingFallback) {
  GfxRenderer renderer;
  renderer.deferredSupported = false;
  renderer.combinedBase = true;
  ReaderUtils::DeferredGrayscalePlanes planes;
  for (int cadence : {-1, 0, 1}) {
    EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  }
  fakeheap::reset(false);
  int cadence = 5;
  EXPECT_FALSE(ReaderUtils::displayDeferredGrayscale(renderer, cadence, planes, [] { return true; }));
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_TRUE(renderer.events.empty());
  EXPECT_EQ(cadence, 5);
}
