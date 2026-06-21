#pragma once

#include "CrossPointSettings.h"

inline bool isPowerButtonActionAvailableOutsideReader(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      return true;
    case CrossPointSettings::SHORT_PWRBTN::IGNORE:
    case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
    case CrossPointSettings::SHORT_PWRBTN::SHORT_PWRBTN_COUNT:
    default:
      return false;
  }
}

void enterDeepSleep(bool fromTimeout = false);
