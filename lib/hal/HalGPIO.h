#pragma once

#include <Arduino.h>
#include <InputManager.h>

#include "AppCapabilities.h"
#include "ButtonInputBuffer.h"

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
  ButtonInputBuffer::Frame buttonFrame;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;
  bool usbStateSampled = false;
  unsigned long lastUsbPollMs = 0;

 public:
  // HAL-owned, normalized multi-touch representation. Activities must not
  // depend on the SDK's controller-specific touch snapshot type.
  struct TouchContact {
    uint8_t id = 0;
    float nx = 0.0f;
    float ny = 0.0f;
  };

  struct TouchSnapshot {
    // Mirror the SDK's bounded capture capacity while keeping app code
    // independent of its controller-specific snapshot type.
    static constexpr uint8_t MAX_CONTACTS = InputManager::MAX_TOUCH_CONTACTS;
    uint8_t count = 0;
    // Actual controller count. This may exceed count when the SDK truncates a
    // frame, allowing firmware to opt into exact cardinalities safely.
    uint8_t reportedCount = 0;
    TouchContact contacts[MAX_CONTACTS];
  };

  struct CompletedMultiTouchSwipe {
    uint8_t contactCount = 0;
    float nxStart = 0.0f;
    float nyStart = 0.0f;
    float nxEnd = 0.0f;
    float nyEnd = 0.0f;
    unsigned long durationMs = 0;
  };

  struct CompletedMultiTouchRotation {
    float degrees = 0.0f;
    float nxCenter = 0.0f;
    float nyCenter = 0.0f;
    unsigned long durationMs = 0;
  };

  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement, the flipped large-step direction in selection activities, and
  // the keyboard's side-gutter reserve. Keyed off the active BoardConfig
  // profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  void startButtonCapture();
  void stopButtonCapture();
  bool prepareForDeepSleep();
  bool hasToggleSwitch() const;
  bool isToggleSwitchOn() const;
  uint32_t buttonSampleTime() const;
  bool hasPendingButtonInput() const;
  // Returns early on a hardware input edge; retains timed polling as fallback.
  bool waitForInput(uint32_t timeoutMs);
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
#if CROSSINK_APP_CAP_TOUCH
  bool hasTouch() const;
  bool supportsMultiTouch() const;
  TouchSnapshot getTouchSnapshot() const;
  bool wasCompletedMultiTouchSwipe(CompletedMultiTouchSwipe& swipe) const;
  bool wasCompletedMultiTouchRotation(CompletedMultiTouchRotation& rotation) const;
  // Capacitive home key under the bezel, reported by the touch controller
  // (e.g. X4 Pro's GT911 key). Tap = short press (fires on release, the primary
  // "home" action); LongPress = held ~700ms (a hold shortcut, e.g. reader menu).
  bool hasHomeKey() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool wasTouchLongPress(float& nx, float& ny) const;
  void suppressTouchContact();
  bool isTouchHeldAt(float& nx, float& ny) const;
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
#else
  constexpr bool hasTouch() const { return false; }
  constexpr bool supportsMultiTouch() const { return false; }
  constexpr TouchSnapshot getTouchSnapshot() const { return {}; }
  constexpr bool wasCompletedMultiTouchSwipe(CompletedMultiTouchSwipe&) const { return false; }
  constexpr bool wasCompletedMultiTouchRotation(CompletedMultiTouchRotation&) const { return false; }
  constexpr bool hasHomeKey() const { return false; }
  constexpr bool wasHomeKeyPressed() const { return false; }
  constexpr bool wasHomeKeyTapped() const { return false; }
  constexpr bool wasHomeKeyLongPressed() const { return false; }
#endif
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify the physical power button through debounce, then require a long
  // hold when the saved shortcut does not permit short-press wake.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup(bool shortPressWakes, uint16_t longHoldMs);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Return the latest loop-owned sample. Before the first update(), fall back
  // to a direct probe so setup-time callers still report external power.
  bool isUsbConnectedCached() const;

  // Whether a cold boot with no USB detected can be trusted to mean a held
  // power button on the active board's power topology.
  bool coldBootImpliesPowerButton() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
