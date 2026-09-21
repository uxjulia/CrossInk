#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct ManualPageTurnRequest {
  bool isForward = true;
  const char* source = "unknown";
};

class ManualPageTurnQueue {
 public:
  static constexpr size_t MAX_PENDING_TURNS = 5;

  enum class EnqueueResult : uint8_t {
    Queued,
    Cancelled,
  };

  // The input loop appends requests while the render task reads this state to
  // decide whether the current page is worth full-quality rendering. The
  // actual entries remain input-loop-owned; only the count crosses that boundary.
  bool hasPending() const { return count.load(std::memory_order_acquire) > 0; }

  EnqueueResult enqueue(const ManualPageTurnRequest request) {
    const uint8_t pendingCount = count.load(std::memory_order_relaxed);
    if ((pendingCount > 0 && turns[0].isForward != request.isForward) ||
        (hasDispatchedTurn && dispatchedTurn.isForward != request.isForward)) {
      clear();
      return EnqueueResult::Cancelled;
    }
    if (pendingCount < MAX_PENDING_TURNS) {
      turns[pendingCount] = request;
      count.store(pendingCount + 1, std::memory_order_release);
    }
    return EnqueueResult::Queued;
  }

  bool takeNext(ManualPageTurnRequest& request) {
    const uint8_t pendingCount = count.load(std::memory_order_relaxed);
    if (pendingCount == 0) return false;

    request = turns[0];
    for (size_t i = 1; i < pendingCount; ++i) {
      turns[i - 1] = turns[i];
    }
    count.store(pendingCount - 1, std::memory_order_release);
    return true;
  }

  void clear() {
    count.store(0, std::memory_order_release);
    hasDispatchedTurn = false;
  }

  void markDispatched(const ManualPageTurnRequest request) {
    dispatchedTurn = request;
    hasDispatchedTurn = true;
  }

  // A turn may already have changed the logical page while its render is
  // still finishing. Keep an immediate opposite input as the next queued
  // turn so it runs after the render lock is released instead of racing it.
  void queueReversalOfDispatched(const ManualPageTurnRequest request) {
    turns[0] = request;
    count.store(1, std::memory_order_release);
    hasDispatchedTurn = false;
  }

  void finishDispatched() { hasDispatchedTurn = false; }

  bool hasDispatched() const { return hasDispatchedTurn; }

  bool dispatchedIsForward() const { return hasDispatchedTurn && dispatchedTurn.isForward; }

  bool dispatchedDirectionMatches(const bool isForward) const {
    return hasDispatchedTurn && dispatchedTurn.isForward == isForward;
  }

  bool dispatchedDirectionOpposes(const bool isForward) const {
    return hasDispatchedTurn && dispatchedTurn.isForward != isForward;
  }

  size_t size() const { return count.load(std::memory_order_acquire); }

 private:
  std::array<ManualPageTurnRequest, MAX_PENDING_TURNS> turns{};
  std::atomic<uint8_t> count{0};
  ManualPageTurnRequest dispatchedTurn{};
  bool hasDispatchedTurn = false;
};

// Coordinates the render task's quality decision with an opposite-direction
// input. A cancellation during the decision lets the current render keep its
// AA and images; a cancellation after either was deferred needs a recovery redraw.
class QueuedTurnRenderingState {
 public:
  void beginDecision() { state.store(DECIDING, std::memory_order_release); }

  bool finishDecision(const bool stillHasSuccessor) {
    uint8_t expected = DECIDING;
    if (!stillHasSuccessor) {
      state.compare_exchange_strong(expected, IDLE, std::memory_order_acq_rel);
      return false;
    }
    return state.compare_exchange_strong(expected, DEFERRED, std::memory_order_acq_rel);
  }

  bool cancelDeferred() { return state.exchange(IDLE, std::memory_order_acq_rel) == DEFERRED; }

  void clear() { state.store(IDLE, std::memory_order_release); }

 private:
  static constexpr uint8_t IDLE = 0;
  static constexpr uint8_t DECIDING = 1;
  static constexpr uint8_t DEFERRED = 2;

  std::atomic<uint8_t> state{IDLE};
};
