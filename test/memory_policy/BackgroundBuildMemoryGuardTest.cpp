#include <gtest/gtest.h>

#include "src/activities/reader/BackgroundBuildMemoryGuard.h"

namespace {
constexpr auto FREE = MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE;
constexpr auto BLOCK = MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC;
}  // namespace

TEST(BackgroundBuildMemoryGuardTest, HealthyHeapDoesNotReclaim) {
  BackgroundBuildMemoryGuard guard;
  EXPECT_FALSE(guard.shouldReclaim({FREE, BLOCK}));
  EXPECT_TRUE(guard.hasHeap({FREE, BLOCK}));
}

TEST(BackgroundBuildMemoryGuardTest, LowFreeOrFragmentationTriggersOnlyOneAttempt) {
  BackgroundBuildMemoryGuard guard;
  EXPECT_TRUE(guard.shouldReclaim({FREE - 1, BLOCK}));
  for (int tick = 0; tick < 100; ++tick) {
    EXPECT_FALSE(guard.shouldReclaim({FREE - 1, BLOCK}));
    EXPECT_FALSE(guard.shouldReclaim({FREE + 10000, BLOCK - 1}));
  }
  EXPECT_FALSE(guard.hasHeap({FREE - 1, BLOCK}));
  EXPECT_FALSE(guard.hasHeap({FREE + 10000, BLOCK - 1}));
}

TEST(BackgroundBuildMemoryGuardTest, RecheckNeedsBothThresholdsAndDoesNotRearm) {
  BackgroundBuildMemoryGuard guard;
  EXPECT_TRUE(guard.shouldReclaim({FREE - 1, BLOCK - 1}));
  EXPECT_FALSE(guard.hasHeap({FREE + 10000, BLOCK - 1}));
  EXPECT_FALSE(guard.hasHeap({FREE - 1, BLOCK + 10000}));
  EXPECT_TRUE(guard.hasHeap({FREE, BLOCK}));
  // A reclaim was enough for one tick, but its allocation consumed that space.
  EXPECT_FALSE(guard.shouldReclaim({FREE - 1, BLOCK}));
}

TEST(BackgroundBuildMemoryGuardTest, HealthyLaterTickRearmsForNextEpisode) {
  BackgroundBuildMemoryGuard guard;
  EXPECT_TRUE(guard.shouldReclaim({FREE, BLOCK - 1}));
  EXPECT_FALSE(guard.shouldReclaim({FREE, BLOCK}));
  EXPECT_TRUE(guard.shouldReclaim({FREE - 1, BLOCK}));
  EXPECT_FALSE(guard.shouldReclaim({FREE - 1, BLOCK}));
}
