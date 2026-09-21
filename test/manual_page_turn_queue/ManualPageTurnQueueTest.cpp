#include <gtest/gtest.h>

#include "ManualPageTurnQueue.h"

namespace {
ManualPageTurnRequest next(const char* source = "front") { return {true, source}; }
ManualPageTurnRequest previous(const char* source = "front") { return {false, source}; }
}  // namespace

TEST(ManualPageTurnQueue, AccumulatesSameDirectionUpToCapacity) {
  ManualPageTurnQueue queue;

  for (size_t i = 0; i < ManualPageTurnQueue::MAX_PENDING_TURNS + 2; ++i) {
    EXPECT_EQ(queue.enqueue(next()), ManualPageTurnQueue::EnqueueResult::Queued);
  }

  EXPECT_EQ(queue.size(), ManualPageTurnQueue::MAX_PENDING_TURNS);
}

TEST(ManualPageTurnQueue, DrainsOneQueuedTurnAtATimeInOrder) {
  ManualPageTurnQueue queue;
  queue.enqueue(next("touch"));
  queue.enqueue(next("side"));
  queue.enqueue(next("power"));

  ManualPageTurnRequest request;
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_TRUE(request.isForward);
  EXPECT_STREQ(request.source, "touch");
  EXPECT_EQ(queue.size(), 2U);
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_STREQ(request.source, "side");
  EXPECT_EQ(queue.size(), 1U);
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_STREQ(request.source, "power");
  EXPECT_FALSE(queue.takeNext(request));
}

TEST(ManualPageTurnQueue, ReportsWhetherTheDispatchedTurnHasASuccessor) {
  ManualPageTurnQueue queue;
  queue.enqueue(next());
  queue.enqueue(next());

  ManualPageTurnRequest request;
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_TRUE(queue.hasPending());
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_FALSE(queue.hasPending());
}

TEST(ManualPageTurnQueue, OppositeDirectionCancelsAndConsumesTheInput) {
  ManualPageTurnQueue queue;
  queue.enqueue(next());
  queue.enqueue(next());

  EXPECT_EQ(queue.enqueue(previous()), ManualPageTurnQueue::EnqueueResult::Cancelled);
  EXPECT_FALSE(queue.hasPending());
}

TEST(ManualPageTurnQueue, CancellationBeforeDrainLeavesNothingToDispatch) {
  ManualPageTurnQueue queue;
  queue.enqueue(previous());

  EXPECT_EQ(queue.enqueue(next()), ManualPageTurnQueue::EnqueueResult::Cancelled);
  ManualPageTurnRequest request;
  EXPECT_FALSE(queue.takeNext(request));
}

TEST(ManualPageTurnQueue, QueuesAReversalOfTheDispatchedTurn) {
  ManualPageTurnQueue queue;
  const ManualPageTurnRequest queuedTurn = next();
  queue.markDispatched(queuedTurn);

  queue.queueReversalOfDispatched(previous("side"));

  EXPECT_TRUE(queue.hasPending());
  EXPECT_FALSE(queue.hasDispatched());
  ManualPageTurnRequest request;
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_FALSE(request.isForward);
  EXPECT_STREQ(request.source, "side");
}

TEST(ManualPageTurnQueue, ReversalReplacesQueuedSuccessorsOfTheDispatchedTurn) {
  ManualPageTurnQueue queue;
  queue.markDispatched(next());
  queue.enqueue(next());

  queue.queueReversalOfDispatched(previous());

  EXPECT_EQ(queue.size(), 1U);
  EXPECT_FALSE(queue.hasDispatched());
  ManualPageTurnRequest request;
  ASSERT_TRUE(queue.takeNext(request));
  EXPECT_FALSE(request.isForward);
}

TEST(QueuedTurnRenderingState, CancellationDuringDecisionKeepsTheCurrentRenderAtFullQuality) {
  QueuedTurnRenderingState state;
  state.beginDecision();

  EXPECT_FALSE(state.cancelDeferred());
  EXPECT_FALSE(state.finishDecision(true));
}

TEST(QueuedTurnRenderingState, CancellationAfterDeferralRequestsRecoveryRedraw) {
  QueuedTurnRenderingState state;
  state.beginDecision();

  ASSERT_TRUE(state.finishDecision(true));
  EXPECT_TRUE(state.cancelDeferred());
}

TEST(QueuedTurnRenderingState, ClearingADeferredQueueRequestsRecoveryRedraw) {
  QueuedTurnRenderingState state;
  state.beginDecision();

  ASSERT_TRUE(state.finishDecision(true));
  EXPECT_TRUE(state.cancelDeferred());
}

TEST(QueuedTurnRenderingState, DiscardingAnOutOfBoundsSuccessorRequestsRecoveryRedraw) {
  QueuedTurnRenderingState state;
  state.beginDecision();

  ASSERT_TRUE(state.finishDecision(true));
  EXPECT_TRUE(state.cancelDeferred());
}

TEST(ManualPageTurnQueue, TracksWhenTheDispatchedTurnOpposesANewDirection) {
  ManualPageTurnQueue queue;
  queue.markDispatched(next());

  EXPECT_TRUE(queue.hasDispatched());
  EXPECT_TRUE(queue.dispatchedIsForward());
  EXPECT_TRUE(queue.dispatchedDirectionMatches(true));
  EXPECT_FALSE(queue.dispatchedDirectionMatches(false));
  EXPECT_TRUE(queue.dispatchedDirectionOpposes(false));
  EXPECT_FALSE(queue.dispatchedDirectionOpposes(true));
}

TEST(ManualPageTurnQueue, ReverseOfForwardDispatchedTurnMatchesPreviousDirection) {
  ManualPageTurnQueue queue;
  queue.markDispatched(next());

  EXPECT_TRUE(queue.dispatchedIsForward());
  EXPECT_FALSE(queue.dispatchedIsForward() == false);
  EXPECT_FALSE(queue.dispatchedDirectionMatches(false));
  EXPECT_TRUE(queue.dispatchedDirectionOpposes(false));
}

TEST(ManualPageTurnQueue, ClearDiscardsPendingAndDispatchedTurns) {
  ManualPageTurnQueue queue;
  queue.enqueue(next());
  queue.markDispatched(next());
  queue.clear();

  EXPECT_FALSE(queue.hasPending());
  EXPECT_FALSE(queue.hasDispatched());
}
