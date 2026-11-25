#pragma once
#include <Arduino.h>
#include <ButtonInputBuffer.h>

#include <algorithm>
class MappedInputManager {
  const ButtonInputBuffer::Frame& frame;

 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down };
  explicit MappedInputManager(const ButtonInputBuffer::Frame& frame) : frame(frame) {}
  bool wasPressed(Button b) const { return frame.pressed & (1u << static_cast<uint8_t>(b)); }
  bool wasReleased(Button b) const { return frame.released & (1u << static_cast<uint8_t>(b)); }
  bool isPressed(Button b) const { return frame.state & (1u << static_cast<uint8_t>(b)); }
  unsigned long getHeldTime() const { return frame.heldMs; }
  uint32_t buttonSampleTime() const { return frame.timestamp; }
};
