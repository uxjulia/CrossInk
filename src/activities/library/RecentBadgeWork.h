#pragma once

#include <cstdint>
#include <optional>

// All state belongs to the current list viewport under RenderLock. Recent
// history has at most 18 slots; no retained paths or background task needed.
class RecentBadgeWork {
 public:
  static constexpr uint8_t CAPACITY = 32;

  void reset() {
    pending = 0;
    redraw = false;
  }

  void queue(const uint16_t slot) {
    if (slot < CAPACITY) pending |= uint32_t{1} << slot;
  }

  [[nodiscard]] std::optional<uint8_t> takeNext() {
    if (pending == 0) return std::nullopt;
    const auto slot = static_cast<uint8_t>(__builtin_ctz(pending));
    pending &= pending - 1;
    return slot;
  }

  [[nodiscard]] bool busy() const { return pending != 0; }

  void resolved(const bool visibleBadge) { redraw |= visibleBadge; }

  // Let the current viewport's badges share one e-ink update. A new
  // viewport consumes the already cached positives in its normal frame.
  bool consumeRedraw() {
    if (busy() || !redraw) return false;
    redraw = false;
    return true;
  }

 private:
  uint32_t pending = 0;
  bool redraw = false;
};
