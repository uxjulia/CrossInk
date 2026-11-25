#pragma once

#include <cstdint>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "util/QuickLockTrigger.h"

// A reader owns one tracker for each logical side/front navigation button. Page turns fire
// on press when no long action is assigned; other short actions wait for release.
// Long actions fire once when the hold threshold is reached.
// Tracking only starts on a press edge, so a button already held while a
// reader activity opens cannot turn into an accidental shortcut.
class ReaderButtonShortcuts {
 public:
  struct Result {
    bool consumed = false;
    bool triggered = false;
    bool longPress = false;
    bool up = false;
    bool front = false;
    uint8_t action = CrossPointSettings::IGNORE;
  };

  static QuickLockTrigger quickLockTrigger(const Result& result) {
    if (result.front) return result.up ? QuickLockTrigger::FrontLeftLong : QuickLockTrigger::FrontRightLong;
    if (result.up) return result.longPress ? QuickLockTrigger::SideUpLong : QuickLockTrigger::SideUpShort;
    return result.longPress ? QuickLockTrigger::SideDownLong : QuickLockTrigger::SideDownShort;
  }

  static bool shouldConsume(const Result& result, const MappedInputManager& input) {
    // Chord Page Turn arrives as an injected front-button release while a side
    // button may still be held. Let that release reach the reader page handler.
    return result.consumed && !input.hasInjectedRelease(MappedInputManager::Button::Right) &&
           !input.hasInjectedRelease(MappedInputManager::Button::Left);
  }

  Result update(MappedInputManager& input, const unsigned long now) {
    Result upResult = updateOne(input, MappedInputManager::Button::Up, true, up_, now, SETTINGS.sideButtonUpShort,
                                SETTINGS.sideButtonUpLong);
    Result downResult = updateOne(input, MappedInputManager::Button::Down, false, down_, now,
                                  SETTINGS.sideButtonDownShort, SETTINGS.sideButtonDownLong);
    Result leftResult = updateOne(input, MappedInputManager::Button::Left, true, left_, now,
                                  CrossPointSettings::PREVIOUS_PAGE, SETTINGS.frontButtonLeftLong);
    Result rightResult = updateOne(input, MappedInputManager::Button::Right, false, right_, now,
                                   CrossPointSettings::PAGE_TURN, SETTINGS.frontButtonRightLong);
    leftResult.front = rightResult.front = true;
    if (leftResult.triggered) return leftResult;
    if (rightResult.triggered) return rightResult;
    if (upResult.triggered) return upResult;
    if (downResult.triggered) return downResult;
    upResult.consumed = upResult.consumed || downResult.consumed || leftResult.consumed || rightResult.consumed;
    return upResult;
  }

 private:
  struct ButtonState {
    unsigned long pressedAt = 0;
    bool tracking = false;
    bool actionFired = false;
  };
  ButtonState up_;
  ButtonState down_;
  ButtonState left_;
  ButtonState right_;

  static Result updateOne(MappedInputManager& input, const MappedInputManager::Button button, const bool up,
                          ButtonState& state, const unsigned long now, const uint8_t shortAction,
                          const uint8_t longAction) {
    Result result;
    result.up = up;
    const bool pressEdge = input.wasPressed(button);
    if (pressEdge) {
      state.pressedAt = now;
      state.tracking = true;
      state.actionFired = false;
    }
    if (!state.tracking) return result;

    result.consumed = true;
    const bool pressed = input.isPressed(button);
    const bool released = input.wasReleased(button);
    const bool immediatePageTurn =
        longAction == CrossPointSettings::IGNORE &&
        (shortAction == CrossPointSettings::PAGE_TURN || shortAction == CrossPointSettings::PREVIOUS_PAGE);
    if (pressEdge && immediatePageTurn) {
      state.actionFired = true;
      result.triggered = true;
      result.action = shortAction;
      // A page turn can open the end-of-book menu before release. Suppress at
      // the input layer so earlier menu handlers cannot reuse this press.
      if (button == MappedInputManager::Button::Left || button == MappedInputManager::Button::Right)
        input.suppressNextFrontRelease(button);
      else
        input.suppressNextSideRelease(button);
    } else if (!state.actionFired && longAction != CrossPointSettings::IGNORE &&
               now - state.pressedAt >= ReaderUtils::SKIP_HOLD_MS && (pressed || released)) {
      state.actionFired = true;
      result.triggered = true;
      result.longPress = true;
      result.action = longAction;
    } else if (released) {
      if (!state.actionFired) {
        result.triggered = true;
        result.action = shortAction;
      }
    }
    if (!pressed) state.tracking = false;
    return result;
  }
};
