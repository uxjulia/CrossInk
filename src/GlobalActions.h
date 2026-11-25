#pragma once

#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

#include "CrossPointSettings.h"
#include "util/QuickLockTrigger.h"

// X4's vendor FULL waveform visibly inverts the whole panel several times.
// Keep its manual shortcut on the clean HALF waveform used before the manual
// refresh change; X3 and every other device retain the explicit full refresh.
inline HalDisplay::RefreshMode manualScreenRefreshMode() {
#if FREEINK_DEVICE_X4
  if (gpio.deviceIsX4()) {
    return HalDisplay::HALF_REFRESH;
  }
#endif
  return HalDisplay::FULL_REFRESH;
}

inline bool isPowerButtonActionAvailableOutsideReader(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP_ONLY:
    case CrossPointSettings::SHORT_PWRBTN::WAKE_ONLY:
    case CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
    case CrossPointSettings::SHORT_PWRBTN::HOME_READER:
    case CrossPointSettings::SHORT_PWRBTN::BACK_HOME:
      return true;
    case CrossPointSettings::SHORT_PWRBTN::IGNORE:
    case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FOCUS_READING:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
    case CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
    case CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS:
    case CrossPointSettings::SHORT_PWRBTN::SHORT_PWRBTN_COUNT:
    default:
      return false;
  }
}

void enterDeepSleep(bool fromTimeout = false);
bool handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN action,
                                   QuickLockTrigger quickLockTrigger = QuickLockTrigger::None);
bool dispatchShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
bool startGlobalSyncProgress(bool networkBootReady = false,
                             uint8_t readerOrientation = CrossPointSettings::ORIENTATION_COUNT);
