#pragma once

#include <InputManager.h>

#include <algorithm>

// Keep button state, edges and held time in the real SDK input decoder.
class MappedInputManager {
  InputManager& input;

 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down };
  explicit MappedInputManager(InputManager& input) : input(input) {}
  bool wasPressed(Button button) const { return input.wasPressed(static_cast<uint8_t>(button)); }
  bool wasReleased(Button button) const { return input.wasReleased(static_cast<uint8_t>(button)); }
  bool isPressed(Button button) const { return input.isPressed(static_cast<uint8_t>(button)); }
  unsigned long getHeldTime() const { return input.getHeldTime(); }
  uint32_t buttonSampleTime() const { return millis(); }
};
