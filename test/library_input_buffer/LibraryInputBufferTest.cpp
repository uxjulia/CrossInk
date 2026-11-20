#include <gtest/gtest.h>

#include "LibraryInputBuffer.h"
#include "LibraryScanSleepToken.h"
#include "RecentBadgeWork.h"

using Type = LibraryInputBuffer::Type;

TEST(LibraryInputBufferTest, NavigationStaysBeforeSelect) {
  LibraryInputBuffer buffer;
  ASSERT_TRUE(buffer.push({Type::Next}));
  ASSERT_TRUE(buffer.push({Type::Previous}));
  ASSERT_TRUE(buffer.push({Type::ConfirmRelease}));
  LibraryInputBuffer::Event event;
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.type, Type::Next);
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.type, Type::Previous);
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.type, Type::ConfirmRelease);
  EXPECT_FALSE(buffer.pop(event));
}

TEST(LibraryInputBufferTest, FullQueueAndWrappedReuseKeepTouchEdgesInOrder) {
  LibraryInputBuffer buffer;
  for (size_t i = 0; i < buffer.CAPACITY; ++i)
    ASSERT_TRUE(buffer.push({Type::TouchPress, static_cast<int16_t>(i), 42}));
  EXPECT_FALSE(buffer.push({Type::ConfirmRelease}));
  LibraryInputBuffer::Event event;
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(buffer.pop(event));
    EXPECT_EQ(event.x, i);
  }
  for (int i = 0; i < 12; ++i) ASSERT_TRUE(buffer.push({Type::TouchRelease, -1, -1}));
  for (size_t i = 12; i < buffer.CAPACITY; ++i) {
    ASSERT_TRUE(buffer.pop(event));
    EXPECT_EQ(event.x, i);
    EXPECT_EQ(event.y, 42);
  }
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(buffer.pop(event));
    EXPECT_EQ(event.type, Type::TouchRelease);
    EXPECT_EQ(event.x, -1);
  }
  EXPECT_FALSE(buffer.pop(event));
}

TEST(LibraryInputBufferTest, ClearingOnNavigationDropsOldScreenEvents) {
  LibraryInputBuffer buffer;
  ASSERT_TRUE(buffer.push({Type::ConfirmRelease}));
  ASSERT_TRUE(buffer.push({Type::TouchRelease, 20, 300}));
  buffer.clear();
  LibraryInputBuffer::Event event;
  EXPECT_FALSE(buffer.pop(event));
  ASSERT_TRUE(buffer.push({Type::BackRelease}));
  ASSERT_TRUE(buffer.pop(event));
  EXPECT_EQ(event.type, Type::BackRelease);
}

TEST(LibraryScanSleepTokenTest, CleanScanSurvivesOnlyOneDeepSleepWake) {
  library::ScanSleepToken token{};
  token.save(true);
  EXPECT_TRUE(token.consume(true));
  EXPECT_FALSE(token.consume(true));
  token.save(true);
  EXPECT_FALSE(token.consume(false));
  EXPECT_FALSE(token.consume(true));
}

TEST(LibraryScanSleepTokenTest, DirtyAndCorruptedScansDoNotSkipReconciliation) {
  library::ScanSleepToken token{};
  token.save(false);
  EXPECT_FALSE(token.consume(true));
  token.save(true);
  token.check ^= 1;
  EXPECT_FALSE(token.consume(true));
  token.save(true);
  token.magic ^= 1;
  EXPECT_FALSE(token.consume(true));
}

TEST(RecentBadgeWorkTest, ResolvesDistinctVisibleSlotsOneAtATimeAndCoalescesRefresh) {
  RecentBadgeWork work;
  constexpr uint16_t slots[] = {4, 4, 7, 12, UINT16_MAX};
  for (const uint16_t slot : slots) work.queue(slot);
  ASSERT_EQ(work.takeNext(), 4);
  work.resolved(true);
  EXPECT_FALSE(work.consumeRedraw());
  ASSERT_EQ(work.takeNext(), 7);
  work.resolved(false);
  EXPECT_FALSE(work.consumeRedraw());
  ASSERT_EQ(work.takeNext(), 12);
  work.resolved(true);
  EXPECT_FALSE(work.takeNext());
  EXPECT_TRUE(work.consumeRedraw());
  EXPECT_FALSE(work.consumeRedraw());
}

TEST(RecentBadgeWorkTest, AViewportChangeDiscardsBothPendingRowsAndAnOldRefresh) {
  RecentBadgeWork work;
  work.queue(2);
  work.queue(3);
  ASSERT_EQ(work.takeNext(), 2);
  work.resolved(true);
  work.reset();
  EXPECT_FALSE(work.takeNext());
  EXPECT_FALSE(work.consumeRedraw());

  work.queue(10);
  ASSERT_EQ(work.takeNext(), 10);
  work.resolved(false);
  EXPECT_FALSE(work.consumeRedraw());
}
