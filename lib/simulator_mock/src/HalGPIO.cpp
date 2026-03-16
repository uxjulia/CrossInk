#include "HalGPIO.h"

#include <SDL2/SDL.h>

#include <atomic>

// Defined in HalDisplay.cpp — set here so all SDL event polling lives in one place.
extern std::atomic<bool> quitRequested;

// Keyboard mapping:
//   BTN_BACK    (0) → Escape
//   BTN_CONFIRM (1) → Return
//   BTN_LEFT    (2) → Left arrow
//   BTN_RIGHT   (3) → Right arrow
//   BTN_UP      (4) → Up arrow
//   BTN_DOWN    (5) → Down arrow
//   BTN_POWER   (6) → P

static constexpr int NUM_BUTTONS = 7;

static const SDL_Scancode buttonScancode[NUM_BUTTONS] = {
    SDL_SCANCODE_ESCAPE,  // BTN_BACK
    SDL_SCANCODE_RETURN,  // BTN_CONFIRM
    SDL_SCANCODE_LEFT,    // BTN_LEFT
    SDL_SCANCODE_RIGHT,   // BTN_RIGHT
    SDL_SCANCODE_UP,      // BTN_UP
    SDL_SCANCODE_DOWN,    // BTN_DOWN
    SDL_SCANCODE_P,       // BTN_POWER
};

static bool pressedThisFrame[NUM_BUTTONS] = {};
static bool releasedThisFrame[NUM_BUTTONS] = {};
static unsigned long buttonPressTime[NUM_BUTTONS] = {};

static int scancodeToButton(SDL_Scancode sc) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttonScancode[i] == sc) return i;
  }
  return -1;
}

void HalGPIO::begin() {}

void HalGPIO::update() {
  // Reset per-frame state
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pressedThisFrame[i] = false;
    releasedThisFrame[i] = false;
  }

  // HalGPIO owns all SDL event polling so keyboard and quit events are never
  // split between two callers (HalDisplay::presentIfNeeded only renders).
  SDL_Event e;
  while (SDL_PollEvent(&e) != 0) {
    if (e.type == SDL_QUIT) {
      quitRequested.store(true);
    } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        pressedThisFrame[btn] = true;
        buttonPressTime[btn] = SDL_GetTicks();
      }
    } else if (e.type == SDL_KEYUP) {
      int btn = scancodeToButton(e.key.keysym.scancode);
      if (btn >= 0) {
        releasedThisFrame[btn] = true;
      }
    }
  }
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  const uint8_t* state = SDL_GetKeyboardState(NULL);
  return state[buttonScancode[buttonIndex]];
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  return pressedThisFrame[buttonIndex];
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  if (buttonIndex >= NUM_BUTTONS) return false;
  return releasedThisFrame[buttonIndex];
}

bool HalGPIO::wasAnyPressed() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (pressedThisFrame[i]) return true;
  }
  return false;
}

bool HalGPIO::wasAnyReleased() const {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (releasedThisFrame[i]) return true;
  }
  return false;
}

unsigned long HalGPIO::getHeldTime() const {
  // Return the longest held time among all currently pressed buttons
  unsigned long now = SDL_GetTicks();
  unsigned long maxHeld = 0;
  const uint8_t* state = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (state[buttonScancode[i]] && buttonPressTime[i] > 0) {
      unsigned long held = now - buttonPressTime[i];
      if (held > maxHeld) maxHeld = held;
    }
  }
  return maxHeld;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const { return WakeupReason::Other; }
bool HalGPIO::isUsbConnected() const { return true; }
