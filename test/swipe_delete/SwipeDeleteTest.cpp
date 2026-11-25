#include <gtest/gtest.h>

#include "components/SwipeDeleteRow.h"

namespace {
namespace fui = freeink::ui;
using Dir = MappedInputManager::SwipeDir;
constexpr fui::ActionId ROW = 1;
constexpr fui::ActionId DELETE = 2;
struct App {
  bool rowPresent = true;
  bool deletePresent = false;
  int cancelledReleases = 0;
  int activations = 0;
  int flashesCleared = 0;
  bool hitPublished(int x, int y, fui::ActionId action, fui::Interaction& hit) {
    if (x < 0 || y != 80 || (action == ROW && !rowPresent) || (action == DELETE && (!deletePresent || x < 400)))
      return false;
    hit.value = 7;
    return true;
  }
  void route(const fui::InputSnapshot& input) {
    if (!input.touchReleased) return;
    if (input.touchX < 0)
      ++cancelledReleases;
    else
      ++activations;
  }
  void clearTapFlash() { ++flashesCleared; }
};
fui::InputSnapshot release(int x, int y) {
  fui::InputSnapshot input{};
  input.touchReleased = true;
  input.touchX = x;
  input.touchY = y;
  return input;
}
}  // namespace

TEST(SwipeDelete, LeftRevealsOnlyTheHitRowAndCancelsItsPress) {
  App app;
  SwipeDeleteRow row;
  EXPECT_TRUE(row.swipe(app, Dir::Left, 200, 80, ROW));
  EXPECT_EQ(row.index, 7);
  EXPECT_EQ(row.reveal(DELETE).index, 7);
  EXPECT_EQ(app.cancelledReleases, 1);
  EXPECT_EQ(app.activations, 0);
  app.rowPresent = false;
  EXPECT_TRUE(row.swipe(app, Dir::Left, 200, 80, ROW));
  EXPECT_EQ(row.index, -1);
}

TEST(SwipeDelete, OutsideTapDismissesWithoutActivatingAnotherRow) {
  App app;
  SwipeDeleteRow row;
  row.index = 7;
  EXPECT_TRUE(row.dismissTap(app, release(100, 80), DELETE));
  EXPECT_EQ(row.index, -1);
  EXPECT_EQ(app.activations, 0);
  EXPECT_EQ(app.cancelledReleases, 1);
}

TEST(SwipeDelete, OnlyTheRevealedDeleteActionCanReachConfirmation) {
  App app;
  app.deletePresent = true;
  SwipeDeleteRow row;
  row.index = 7;
  EXPECT_FALSE(row.dismissTap(app, release(450, 80), DELETE));
  EXPECT_EQ(row.index, 7);
  row.index = 8;
  EXPECT_TRUE(row.dismissTap(app, release(450, 80), DELETE));
  EXPECT_EQ(row.index, -1);
}

TEST(SwipeDelete, RightClosesWhileVerticalGesturesContinueScrolling) {
  App app;
  SwipeDeleteRow row;
  row.index = 7;
  EXPECT_TRUE(row.swipe(app, Dir::Right, 200, 80, ROW));
  EXPECT_EQ(row.index, -1);
  row.index = 7;
  EXPECT_FALSE(row.swipe(app, Dir::Up, 200, 80, ROW));
  EXPECT_EQ(row.index, -1);
}

TEST(SwipeDelete, OffTargetReleaseFromQueuedSwipeDoesNotDismissNewReveal) {
  App app;
  SwipeDeleteRow row;
  row.index = 7;
  EXPECT_FALSE(row.dismissTap(app, release(-1, -1), DELETE));
  EXPECT_EQ(row.index, 7);
}
