#include <gtest/gtest.h>

#include "TouchActionButtons.h"

namespace {
constexpr int padding = TouchActionButtons::kDefaultGap / 2;
const auto actions = TouchActionButtons::vertical(Rect{20, 656, 440, 128}, 2);
}  // namespace

TEST(TouchActionButtons, AcceptsStickyLaterContactWithExplicitPadding) {
  // Recorded from Sticky: a valid tap just below Later's visible bottom at y=784.
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 207, 789), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 207, 789, padding), 1);
}

TEST(TouchActionButtons, PaddingStaysBoundedAndDoesNotMergeActions) {
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 719, padding), 0);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 720, padding), 1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 791, padding), 1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 792, padding), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 11, 756, padding), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 12, 756, padding), 1);
}

TEST(TouchActionButtons, PaddingDoesNotActivateAnEmptyLayout) {
  const auto empty = TouchActionButtons::vertical(Rect{}, 2);
  EXPECT_EQ(TouchActionButtons::indexAt(empty, 0, 0, padding), -1);
}
