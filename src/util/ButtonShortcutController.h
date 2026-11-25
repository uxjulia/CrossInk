#pragma once

#include <cstdint>

#include "QuickLockState.h"
#include "QuickLockTrigger.h"

// Owns the Power + side-button chord latches. Once a chord fires, both buttons
// must be released before another shortcut can fire from the same hold.
class ButtonShortcutController {
 public:
  enum class ChordAction : uint8_t {
    Screenshot = 0,
    QuickLock = 1,
    Disabled = 4,
    Sleep = 5,
    PageTurn = 6,
    ToggleBookmark = 7,
    ReadingStats = 8,
    MarkFinished = 9,
    ForceRefresh = 10,
    ToggleFont = 11,
    ToggleGuideDots = 12,
    ToggleFocusReading = 13,
    CyclePageTurn = 14,
    SyncProgress = 15,
    FileTransfer = 16,
    CalibreWireless = 17,
    JoinNetwork = 18,
    CreateHotspot = 19,
    ToggleDarkMode = 20,
    Footnotes = 21,
    FileBrowser = 22,
    CreateClipping = 23,
    LookupWord = 24,
    ToggleHomeButton = 25,
    QuickActions = 26,
    ToggleFrontlight = 27,
    ToggleTouchscreen = 28,
    PreviousPage = 29,
    NearbyPositionSync = 30,
    Library = 31,
    HomeReader = 32,
    BackHome = 33,
    SelectChapter = 34,
    RotateFlip = 35,
    TogglePageTurnGestures = 36,
  };

  enum class Event : uint8_t { None, QuickLockChanged, Screenshot, PageTurn, ConfiguredAction, TouchscreenEscapeHatch };

  struct Result {
    Event event = Event::None;
    bool consumeInput = false;
    ChordAction action = ChordAction::Disabled;
  };

  Result update(uint32_t nowMs, bool powerPressed, bool chordButtonPressed, bool shortPowerRelease,
                bool quickLockOnShortPower, ChordAction action, bool modalOwnsInput = false) {
    if (chordActive_) {
      if (!powerPressed && !chordButtonPressed) chordActive_ = false;
      return {Event::None, true};
    }

    if (powerPressed && chordButtonPressed && (modalOwnsInput || action != ChordAction::Disabled)) {
      chordActive_ = true;
      if (modalOwnsInput) return {Event::None, true};
      if (quickLockState_.isLocked() && !canRunChordWhileQuickLocked(action, QuickLockTrigger::PowerUp)) {
        return {Event::None, true};
      }
      return dispatchChord(nowMs, action, QuickLockTrigger::PowerUp);
    }

    if (shortPowerRelease) {
      if ((quickLockState_.isLocked() && quickLockTrigger_ == QuickLockTrigger::ShortPower) ||
          (!quickLockState_.isLocked() && quickLockOnShortPower)) {
        toggleQuickLock(nowMs, QuickLockTrigger::ShortPower);
        return {Event::QuickLockChanged, true};
      }
    }
    return {Event::None, quickLockState_.isLocked()};
  }

  Result updatePowerDown(bool powerPressed, bool downPressed, bool blockAction) {
    if (powerDownChordActive_) {
      if (!powerPressed && !downPressed) powerDownChordActive_ = false;
      return {Event::None, true};
    }

    if (!powerPressed || !downPressed) return {};

    powerDownChordActive_ = true;
    return blockAction ? Result{Event::None, true} : Result{Event::Screenshot, true};
  }

  Result updateUpDown(uint32_t nowMs, bool upPressed, bool downPressed, ChordAction action, bool touchscreenEscapeHatch,
                      bool modalOwnsInput = false) {
    if (upDownChordActive_) {
      if (!upPressed && !downPressed) upDownChordActive_ = false;
      return {Event::None, true};
    }

    if (!upPressed || !downPressed || (!modalOwnsInput && !touchscreenEscapeHatch && action == ChordAction::Disabled)) {
      // An idle Up + Down controller must not preempt the reader's permitted
      // Quick Lock unlock trigger (for example, long-press Menu or Back).
      // Locked input is consumed by the main Quick Lock path after it has
      // given that trigger a chance to run.
      return {};
    }

    upDownChordActive_ = true;
    if (modalOwnsInput) return {Event::None, true};
    if (quickLockState_.isLocked() && !canRunChordWhileQuickLocked(action, QuickLockTrigger::UpDown)) {
      return {Event::None, true};
    }
    if (touchscreenEscapeHatch) return {Event::TouchscreenEscapeHatch, true};
    return dispatchChord(nowMs, action, QuickLockTrigger::UpDown);
  }

  bool isQuickLocked() const { return quickLockState_.isLocked(); }
  QuickLockTrigger quickLockTrigger() const { return quickLockTrigger_; }
  bool isChordActive() const { return chordActive_; }
  void toggleQuickLock(uint32_t nowMs, QuickLockTrigger trigger, bool requireRelease = false) {
    const bool locked = quickLockState_.toggle(nowMs);
    quickLockTrigger_ = locked ? trigger : QuickLockTrigger::None;
    unlockTriggerReleased_ = !locked || !requireRelease;
    sideUpUnlock_ = {};
    sideDownUnlock_ = {};
  }
  bool tryUnlockSide(uint32_t nowMs, bool upPressed, bool upPressEdge, bool upReleaseEdge, bool upHasLongAction,
                     bool downPressed, bool downPressEdge, bool downReleaseEdge, bool downHasLongAction,
                     uint32_t longPressMs) {
    if (!quickLockState_.isLocked() || quickLockTrigger_ == QuickLockTrigger::FrontLeftLong ||
        quickLockTrigger_ == QuickLockTrigger::FrontRightLong)
      return false;
    return tryUnlockOneSide(nowMs, upPressed, upPressEdge, upReleaseEdge, upHasLongAction, longPressMs,
                            QuickLockTrigger::SideUpShort, QuickLockTrigger::SideUpLong, sideUpUnlock_) ||
           tryUnlockOneSide(nowMs, downPressed, downPressEdge, downReleaseEdge, downHasLongAction, longPressMs,
                            QuickLockTrigger::SideDownShort, QuickLockTrigger::SideDownLong, sideDownUnlock_);
  }
  bool tryUnlockFront(uint32_t nowMs, bool leftPressed, bool leftEdge, bool leftReleased, bool rightPressed,
                      bool rightEdge, bool rightReleased, uint32_t longPressMs) {
    if (!quickLockState_.isLocked() ||
        (quickLockTrigger_ != QuickLockTrigger::FrontLeftLong && quickLockTrigger_ != QuickLockTrigger::FrontRightLong))
      return false;
    return tryUnlockOneSide(nowMs, leftPressed, leftEdge, leftReleased, true, longPressMs, QuickLockTrigger::None,
                            QuickLockTrigger::FrontLeftLong, sideUpUnlock_) ||
           tryUnlockOneSide(nowMs, rightPressed, rightEdge, rightReleased, true, longPressMs, QuickLockTrigger::None,
                            QuickLockTrigger::FrontRightLong, sideDownUnlock_);
  }
  bool tryUnlockLongPower(uint32_t nowMs, bool longPowerPressed) {
    if (!quickLockState_.isLocked() || quickLockTrigger_ != QuickLockTrigger::LongPower) return false;
    if (!unlockTriggerReleased_) {
      unlockTriggerReleased_ = !longPowerPressed;
      return false;
    }
    if (!longPowerPressed) return false;
    toggleQuickLock(nowMs, QuickLockTrigger::LongPower, true);
    return true;
  }
  bool tryUnlockWithTrigger(uint32_t nowMs, QuickLockTrigger trigger) {
    if (trigger == QuickLockTrigger::None || trigger == QuickLockTrigger::LongPower || !quickLockState_.isLocked() ||
        quickLockTrigger_ != trigger) {
      return false;
    }
    toggleQuickLock(nowMs, trigger);
    return true;
  }
  bool shouldQuickLockSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    return quickLockState_.shouldSleep(nowMs, timeoutMs);
  }

 private:
  struct SideUnlockState {
    uint32_t pressedAt = 0;
    bool tracking = false;
    bool longHandled = false;
  };

  bool tryUnlockOneSide(uint32_t nowMs, bool pressed, bool pressEdge, bool releaseEdge, bool hasLongAction,
                        uint32_t longPressMs, QuickLockTrigger shortTrigger, QuickLockTrigger longTrigger,
                        SideUnlockState& state) {
    if (pressEdge) {
      state = {nowMs, true, false};
    }
    if (!state.tracking) return false;
    if (!state.longHandled && hasLongAction && nowMs - state.pressedAt >= longPressMs && (pressed || releaseEdge)) {
      state.longHandled = true;
      if (quickLockTrigger_ == longTrigger) {
        toggleQuickLock(nowMs, longTrigger);
        return true;
      }
    } else if (releaseEdge && !state.longHandled && quickLockTrigger_ == shortTrigger) {
      toggleQuickLock(nowMs, shortTrigger);
      return true;
    }
    if (!pressed) state.tracking = false;
    return false;
  }

  bool canRunChordWhileQuickLocked(ChordAction action, QuickLockTrigger trigger) const {
    return action == ChordAction::QuickLock && quickLockTrigger_ == trigger;
  }

  Result dispatchChord(uint32_t nowMs, ChordAction action, QuickLockTrigger trigger) {
    switch (action) {
      case ChordAction::Screenshot:
        return {Event::Screenshot, true};
      case ChordAction::QuickLock:
        toggleQuickLock(nowMs, trigger);
        return {Event::QuickLockChanged, true};
      case ChordAction::PageTurn:
        return {Event::PageTurn, true};
      case ChordAction::Disabled:
        return {Event::None, true};
      default:
        return {Event::ConfiguredAction, true, action};
    }
  }

  QuickLockState quickLockState_;
  QuickLockTrigger quickLockTrigger_ = QuickLockTrigger::None;
  bool chordActive_ = false;
  bool powerDownChordActive_ = false;
  bool upDownChordActive_ = false;
  bool unlockTriggerReleased_ = true;
  SideUnlockState sideUpUnlock_;
  SideUnlockState sideDownUnlock_;
};
