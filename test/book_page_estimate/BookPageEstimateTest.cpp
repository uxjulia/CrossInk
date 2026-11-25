#include <gtest/gtest.h>

#include <cmath>

#include "BookPageEstimate.h"

TEST(BookPageEstimate, ChapterOpeningCannotReplaceEstablishedBookDensity) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 100, 99, true);
  const float before = estimate.remainingPages(500000, 0, 1);
  // The provisional chapter estimate more than doubles between the first two
  // pages: the original whole-book formula jumped from 200 to 499 pages.
  estimate.observe(1, 100000, 40, 1, 0, false);
  EXPECT_FLOAT_EQ(estimate.remainingPages(500000, 0, 1), before);
  estimate.observe(1, 100000, 100, 2, 1, false);
  EXPECT_NEAR(estimate.remainingPages(500000, 1, 2), before - 1, 0.001f);
}

TEST(BookPageEstimate, CompletedChapterOpeningAlsoRetainsPrior) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 100, 99, true);
  estimate.observe(1, 100000, 40, 40, 0, true);
  EXPECT_FLOAT_EQ(estimate.pagesPerByte(), 0.001f);
  estimate.observe(1, 100000, 40, 40, 1, true);
  EXPECT_GT(estimate.pagesPerByte(), 0.00097f);
}

TEST(BookPageEstimate, ColdIncrementalPrefixWaitsForEnoughLayout) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 40, 1, 0, false);
  EXPECT_EQ(estimate.pagesPerByte(), 0);
  estimate.observe(0, 100000, 100, 8, 0, false);
  EXPECT_FLOAT_EQ(estimate.pagesPerByte(), 0.001f);
}

TEST(BookPageEstimate, FinalizedShortChapterCanBootstrap) {
  BookPageEstimate estimate;
  estimate.observe(0, 1000, 2, 2, 0, true);
  EXPECT_FLOAT_EQ(estimate.pagesPerByte(), 0.002f);
}

TEST(BookPageEstimate, NewDensityGainsWeightAsReadingContinues) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 100, 99, true);
  estimate.observe(1, 100000, 200, 40, 1, false);
  const float early = estimate.pagesPerByte();
  estimate.observe(1, 100000, 200, 80, 32, false);
  EXPECT_GT(estimate.pagesPerByte(), early);
  EXPECT_NEAR(estimate.pagesPerByte(), 0.0015f, 0.000001f);
}

TEST(BookPageEstimate, RedrawsAndRevisitsDoNotMultiplySamples) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 100, 32, true);
  estimate.observe(1, 100000, 200, 200, 16, true);
  const float before = estimate.pagesPerByte();
  for (int i = 0; i < 100; ++i) estimate.observe(1, 100000, 200, 200, 16, true);
  estimate.observe(1, 100000, 200, 200, 1, true);
  EXPECT_FLOAT_EQ(estimate.pagesPerByte(), before);
}

TEST(BookPageEstimate, ReadingProgressSubtractsRealPagesEvenIfIndexGrows) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 100, 32, true);
  for (int page = 0; page < 7; ++page) {
    estimate.observe(1, 100000, 40 + page * 10, page + 1, page, false);
    EXPECT_NEAR(estimate.remainingPages(500000, page, page + 1), 500 - page, 0.001f);
  }
  estimate.observe(1, 100000, 100, 8, 7, false);
  EXPECT_NEAR(estimate.remainingPages(500000, 7, 8), 493, 0.001f);
}

TEST(BookPageEstimate, AlreadyIndexedPagesAreALowerBound) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 10, 10, 9, true);
  EXPECT_EQ(estimate.remainingPages(100000, 20, 50), 30);
}

TEST(BookPageEstimate, InvalidSamplesAndUnseededQueriesAreSafe) {
  BookPageEstimate estimate;
  estimate.observe(-1, 1000, 10, 10, 1, true);
  estimate.observe(0, 0, 10, 10, 1, true);
  estimate.observe(0, 1000, 0, 10, 1, true);
  estimate.observe(0, 1000, 10, 0, 1, true);
  estimate.observe(0, 1000, 10, 10, -1, true);
  EXPECT_EQ(estimate.pagesPerByte(), 0);
  EXPECT_EQ(estimate.remainingPages(10000, 0, 1), 0);
}

TEST(BookPageEstimate, BoundedHistoryRemainsFiniteAndCanReset) {
  BookPageEstimate estimate;
  for (int i = 0; i < 100; ++i) estimate.observe(i, UINT32_MAX, UINT16_MAX, UINT16_MAX, 60000, true);
  EXPECT_TRUE(std::isfinite(estimate.remainingPages(UINT64_MAX, 1, 1)));
  EXPECT_GT(estimate.pagesPerByte(), 0);
  estimate = {};
  EXPECT_EQ(estimate.pagesPerByte(), 0);
  EXPECT_LE(sizeof(estimate), 104U);
}

TEST(BookPageEstimate, EvictedChapterDoesNotDonateItsWeightToNewChapter) {
  BookPageEstimate estimate;
  for (int i = 0; i < 8; ++i) estimate.observe(i, 100000, 100, 100, 32, true);
  estimate.observe(8, 100000, 200, 200, 1, true);
  EXPECT_LT(estimate.pagesPerByte(), 0.00101f);
}

TEST(BookPageEstimate, IncrementalRevisionOnSamePageDoesNotRewriteDensity) {
  BookPageEstimate estimate;
  estimate.observe(0, 100000, 100, 40, 32, false);
  for (int i = 0; i < 100; ++i) estimate.observe(0, 100000, 200, 45, 32, false);
  EXPECT_FLOAT_EQ(estimate.pagesPerByte(), 0.001f);
  estimate.observe(0, 100000, 200, 46, 33, false);
  EXPECT_NEAR(estimate.pagesPerByte(), 0.00103125f, 0.0000001f);
}
