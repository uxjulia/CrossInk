#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "Epub/ReferencePageNavigation.h"
#include "StablePageSelectionModel.h"

namespace {
struct Span {
  uint32_t wordStart = 0;
  uint32_t wordCount = 0;
};

struct PageStart {
  uint32_t visibleTextOffset = 0;
};

TEST(StablePageNavigation, ResolvesFirstMiddleAndLastPagesAcrossSpines) {
  const std::array<Span, 3> spans = {{{0, 600}, {600, 600}, {1200, 600}}};
  int spine = -1;
  float progress = -1.0f;

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(1, 6, 1800, 300, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_EQ(spine, 0);
  EXPECT_FLOAT_EQ(progress, 0.0f);

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(3, 6, 1800, 300, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_EQ(spine, 1);
  EXPECT_FLOAT_EQ(progress, 0.0f);

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(6, 6, 1800, 300, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_EQ(spine, 2);
  EXPECT_FLOAT_EQ(progress, 0.5f);
}

TEST(StablePageNavigation, ClampsOutOfRangePages) {
  const std::array<Span, 1> spans = {{{0, 1000}}};
  int spine = -1;
  float progress = -1.0f;

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(0, 4, 1000, 250, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_FLOAT_EQ(progress, 0.0f);

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(99, 4, 1000, 250, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_FLOAT_EQ(progress, 0.75f);
}

TEST(StablePageNavigation, SupportsCharacterBasedReferenceUnits) {
  const std::array<Span, 2> spans = {{{0, 3000}, {3000, 3000}}};
  int spine = -1;
  float progress = -1.0f;

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(4, 4, 6000, 1500, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_EQ(spine, 1);
  EXPECT_FLOAT_EQ(progress, 0.5f);
}

TEST(StablePageNavigation, HandlesMissingDataAndMalformedGapsSafely) {
  const std::array<Span, 3> spans = {{{0, 0}, {0, 100}, {200, 100}}};
  int spine = -1;
  float progress = -1.0f;

  EXPECT_FALSE(
      EpubNavigation::resolveReferencePageToSpineProgress(1, 0, 300, 100, spans.data(), spans.size(), spine, progress));
  ASSERT_TRUE(
      EpubNavigation::resolveReferencePageToSpineProgress(2, 3, 300, 150, spans.data(), spans.size(), spine, progress));
  EXPECT_EQ(spine, 2);
  EXPECT_FLOAT_EQ(progress, 0.0f);
}

TEST(StablePageNavigation, RequiresAtLeastOneUsableSpineRange) {
  const std::array<Span, 2> emptySpans = {{{0, 0}, {200, 0}}};
  const std::array<Span, 2> outOfBoundsSpans = {{{300, 50}, {400, 50}}};

  EXPECT_FALSE(EpubNavigation::hasResolvableReferencePageRanges(300, emptySpans.data(), emptySpans.size()));
  EXPECT_FALSE(EpubNavigation::hasResolvableReferencePageRanges(300, outOfBoundsSpans.data(), outOfBoundsSpans.size()));
}

TEST(StablePageNavigation, FallsBackToLastReadableSpineForInconsistentTotals) {
  const std::array<Span, 2> spans = {{{0, 100}, {100, 100}}};
  int spine = -1;
  float progress = -1.0f;

  ASSERT_TRUE(EpubNavigation::resolveReferencePageToSpineProgress(10, 10, 1000, 100, spans.data(), spans.size(), spine,
                                                                  progress));
  EXPECT_EQ(spine, 1);
  EXPECT_FLOAT_EQ(progress, 1.0f);
}

TEST(StablePageSelection, ClampsStepsAndMapsSliderEndpoints) {
  EXPECT_EQ(clampStablePage(0, 300), 1u);
  EXPECT_EQ(clampStablePage(301, 300), 300u);
  EXPECT_EQ(adjustStablePage(1, -10, 300), 1u);
  EXPECT_EQ(adjustStablePage(295, 10, 300), 300u);
  EXPECT_EQ(stablePageFromPermille(0, 300), 1u);
  EXPECT_EQ(stablePageFromPermille(1000, 300), 300u);
  EXPECT_EQ(stablePageToPermille(1, 300), 0);
  EXPECT_EQ(stablePageToPermille(300, 300), 1000);
}

TEST(StablePageNavigation, UsesSourceOffsetsForUnevenRenderedPages) {
  const std::array<PageStart, 4> pageStarts = {{{0}, {50}, {60}, {300}}};

  // 65% through the source belongs to page 2 even though 65% of four rendered
  // pages would round down to page 2 only by coincidence. The second assertion
  // exercises a target where rendered-page percentage would choose page 1.
  EXPECT_EQ(EpubNavigation::resolveReferenceTargetToRenderedPage(65, 100, 100, pageStarts), 2);
  EXPECT_EQ(EpubNavigation::resolveReferenceTargetToRenderedPage(55, 100, 100, pageStarts), 1);
}
}  // namespace
