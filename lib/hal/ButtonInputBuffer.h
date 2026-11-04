#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The sampler owns writes; callers serialize access while transferring a frame.
// Frames freeze both edges and held times, so a slow UI cannot age a tap into a hold.
class ButtonInputBuffer {
 public:
  static constexpr uint8_t BUTTON_COUNT = 7;
  static constexpr uint8_t POWER_MASK = 1u << 6;
  static constexpr size_t CAPACITY = 64;
  static constexpr uint32_t PRESS_MS = 6;
  static constexpr uint32_t RELEASE_MS = 12;
  struct Frame {
    uint32_t timestamp = 0;
    uint32_t heldMs = 0;
    uint32_t powerHeldMs = 0;
    uint8_t state = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
  };

  void reset(uint8_t state, uint32_t now) {
    head = count = 0;
    lost = lastHeldMs = lastPowerHeldMs = lastPulseAt = 0;
    lastPulse = 0;
    overflow = false;
    delivered = state;
    raw = stable = state;
    candidates.fill(now);
    pressedAt = powerPressedAt = emptyAt = lastHoldAt = now;
  }

  // Returns true for a debounced edge. The SDK supplies raw button decoding;
  // debounce here remains independent of UI/render scheduling.
  bool sample(uint8_t state, uint32_t now, uint32_t decodedHeldMs = 0, uint32_t decodedPowerMs = 0) {
    const auto changed = static_cast<uint8_t>(raw ^ state);
    for (uint8_t button = 0; button < BUTTON_COUNT; ++button) {
      if (changed & (1u << button)) candidates[button] = now;
    }
    if (state == 0 && raw != 0) emptyAt = now;
    raw = state;
    uint8_t next = stable;
    uint8_t incoming = 0;
    uint32_t pressAge = decodedHeldMs;
    for (uint8_t button = 0; button < BUTTON_COUNT; ++button) {
      const uint8_t bit = 1u << button;
      if (!((raw ^ stable) & bit)) continue;
      const bool down = raw & bit;
      const uint32_t age = now - candidates[button];
      if (age < (down ? PRESS_MS : RELEASE_MS)) continue;
      next ^= bit;
      if (down) {
        incoming |= bit;
        if (age > pressAge) pressAge = age;
      }
    }
    // A distinct stable press confirms any already-absent keys are released.
    // Otherwise the longer release filter could fabricate a chord while the
    // user moves directly from one button to another.
    if (incoming) next &= raw;
    if (next != stable) {
      const uint8_t presses = next & ~stable;
      const uint8_t releases = stable & ~next;
      if (!stable && next) pressedAt = now - pressAge;
      if (stable && !next) lastHeldMs = emptyAt - pressedAt;
      if (presses & POWER_MASK) {
        const uint32_t age = now - candidates[6];
        powerPressedAt = now - (decodedPowerMs > age ? decodedPowerMs : age);
      }
      if (releases & POWER_MASK) lastPowerHeldMs = candidates[6] - powerPressedAt;
      stable = next;
      Frame frame = snapshot(now);
      frame.pressed = presses;
      frame.released = releases;
      push(frame);
      lastHoldAt = now;
      return true;
    }
    // One coalesced held frame preserves long-press information during a stall
    // without filling the FIFO with every sampler tick.
    if (stable && now - lastHoldAt >= 50) {
      push(snapshot(now));
      lastHoldAt = now;
    }
    return false;
  }

  // Shared Confirm/Power backends can report an entire short click in one poll.
  void pulse(uint8_t buttons, uint32_t heldMs, uint32_t now) {
    if (!buttons || heldMs < PRESS_MS) return;
    if (buttons == lastPulse && now - lastPulseAt < RELEASE_MS) return;
    lastPulse = buttons;
    lastPulseAt = now;
    Frame down = snapshot(now);
    down.state |= buttons;
    down.pressed = buttons;
    if (!stable) down.heldMs = heldMs;
    push(down);
    Frame up = snapshot(now);
    up.released = buttons;
    if (!stable) up.heldMs = heldMs;
    push(up);
  }

  Frame take(uint32_t now) {
    if (count) {
      const Frame frame = frames[head];
      head = (head + 1) % CAPACITY;
      --count;
      delivered = frame.state;
      return frame;
    }
    Frame frame = snapshot(now);
    if (overflow) {
      // Preserve the queued prefix, then release any stale held state. Never
      // synthesize clicks for input beyond the explicitly bounded capacity.
      frame.released = delivered & ~stable;
      overflow = false;
    }
    delivered = stable;
    return frame;
  }

  Frame snapshot(uint32_t now) const {
    Frame frame;
    frame.timestamp = now;
    frame.state = stable;
    frame.heldMs = stable ? (raw ? now : emptyAt) - pressedAt : lastHeldMs;
    frame.powerHeldMs =
        (stable & POWER_MASK) ? ((raw & POWER_MASK) ? now : candidates[6]) - powerPressedAt : lastPowerHeldMs;
    return frame;
  }
  bool pending() const { return count || overflow; }
  bool debouncing() const { return raw != stable; }
  bool held() const { return stable != 0; }
  uint32_t lostEdges() const { return lost; }

 private:
  std::array<Frame, CAPACITY> frames{};
  std::array<uint32_t, BUTTON_COUNT> candidates{};
  uint32_t pressedAt = 0, powerPressedAt = 0, emptyAt = 0;
  uint32_t lastHeldMs = 0, lastPowerHeldMs = 0, lastHoldAt = 0;
  uint32_t lastPulseAt = 0, lost = 0;
  size_t head = 0, count = 0;
  uint8_t raw = 0, stable = 0, delivered = 0, lastPulse = 0;
  bool overflow = false;

  void push(const Frame& frame) {
    const bool edge = frame.pressed || frame.released;
    if (overflow) {
      if (edge) ++lost;
      return;
    }
    if (count) {
      Frame& last = frames[(head + count - 1) % CAPACITY];
      if (!edge && !last.pressed && !last.released && last.state == frame.state) {
        last = frame;
        return;
      }
      if (count == CAPACITY && edge && !last.pressed && !last.released) {
        last = frame;
        return;
      }
    }
    if (count == CAPACITY) {
      if (edge) {
        ++lost;
        overflow = true;
      }
      return;
    }
    frames[(head + count) % CAPACITY] = frame;
    ++count;
  }
};
