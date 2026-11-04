#pragma once

// Boot establishes a baseline; subsequent debounced movements are toggle actions.
class SlideSwitchToggle {
 public:
  bool update(const bool on) {
    const bool moved = initialized && on != lastPosition;
    lastPosition = on;
    initialized = true;
    return moved;
  }

 private:
  bool initialized = false;
  bool lastPosition = false;
};
