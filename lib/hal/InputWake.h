#pragma once

#include <cstdint>

#include "ButtonInputBuffer.h"

class InputManager;

// Firmware-owned wake signal and physical-button capture. Call begin() after
// selecting the board and finishing input/touch pin initialization. No decoding or
// input-state mutation happens in the interrupt; HalGPIO delivers queued frames.
// The signal and handlers live for the lifetime of the firmware.
class InputWake {
 public:
  // Digital and shared buttons, plus toggle switches. GT911's IRQ wakes touch; other
  // touch backends retain their existing IRQ ownership and periodic polling.
  // Failure leaves wait() using its timed polling fallback.
  static bool begin();

  // Wait at most timeoutMs, returning true if an input line changed. Changes
  // before the wait remain latched, including between update() and wait().
  // X3/X4 ADC ladders use short raw checks against the last decoded state,
  // without running the full reader loop or consuming input/gesture events.
  static bool wait(uint32_t timeoutMs, InputManager& input);

  // Start after board setup and wake verification. A button-only SDK decoder
  // keeps capturing while the UI is busy; touch remains main-loop owned.
  static bool startCapture();
  static void stopCapture();
  static bool capturing();
  // Non-consuming snapshot for optional work that should yield to button input.
  static bool hasPendingButtons();
  // Non-consuming IRQ latch, including GT911 touch arriving during optional work.
  static bool hasPendingEdge();
  static ButtonInputBuffer::Frame takeFrame();
  static void setSharedConfirmPowerShortPressEmitsPower(bool enabled);
};
