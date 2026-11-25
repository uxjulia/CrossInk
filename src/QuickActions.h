#pragma once

#include <CrossInkHalFrontlight.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <array>
#include <functional>

#include "CrossPointSettings.h"

class OptionPopup;

// One source of truth for the shortcut that opens Quick Actions.  UI and web
// settings call this after changing an action, so the persisted state cannot
// end up with two physical gestures claiming the same menu.
namespace QuickActions {
// Keep existing values stable because the selected trigger is persisted.
enum class Trigger : uint8_t {
  None = 0,
  ShortPower,
  LongPower,
  LongBack,
  LongMenu,
  PowerUp,
  TapHome,
  LongPressHome,
  DoubleTapHome,
  UpDown,
  SideUpShort,
  SideUpLong,
  SideDownShort,
  SideDownLong,
  FrontLeftLong,
  FrontRightLong,
};

inline constexpr std::array<StrId, CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT> actionLabels = {
    StrId::STR_IGNORE,
    StrId::STR_SLEEP,
    StrId::STR_PAGE_TURN,
    StrId::STR_FORCE_REFRESH,
    StrId::STR_CHANGE_FONT,
    StrId::STR_GUIDE_READING,
    StrId::STR_FOCUS_READING,
    StrId::STR_TOGGLE_BOOKMARK,
    StrId::STR_SYNC_BOOK,
    StrId::STR_MARK_FINISHED,
    StrId::STR_READING_STATS,
    StrId::STR_SCREENSHOT_BUTTON,
    StrId::STR_AUTO_TURN_INTERVAL_SECONDS,
    StrId::STR_FILE_TRANSFER,
    StrId::STR_TILT_PAGE_TURN,
    StrId::STR_READER_DARK_MODE,
    StrId::STR_FOOTNOTES,
    StrId::STR_BROWSE_FILES,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CREATE_HOTSPOT,
    StrId::STR_SAVE_CLIPPING,
    StrId::STR_LOOKUP};

// Shared display order for shortcut pickers. The values remain the persisted
// SHORT_PWRBTN IDs; only their presentation order is centralized here.
inline constexpr std::array<CrossPointSettings::SHORT_PWRBTN, 36> shortcutActionOrder = {
    CrossPointSettings::IGNORE,
    CrossPointSettings::SLEEP,
    CrossPointSettings::PAGE_TURN,
    CrossPointSettings::PREVIOUS_PAGE,
    CrossPointSettings::SELECT_CHAPTER,
    CrossPointSettings::ROTATE_FLIP,
    CrossPointSettings::TOGGLE_BOOKMARK,
    CrossPointSettings::READING_STATS,
    CrossPointSettings::MARK_FINISHED,
    CrossPointSettings::FORCE_REFRESH,
    CrossPointSettings::TOGGLE_FONT,
    CrossPointSettings::TOGGLE_GUIDE_DOTS,
    CrossPointSettings::TOGGLE_FOCUS_READING,
    CrossPointSettings::CYCLE_PAGE_TURN,
    CrossPointSettings::TOGGLE_TILT_PAGE_TURN,
    CrossPointSettings::SYNC_PROGRESS,
    CrossPointSettings::NEARBY_POSITION_SYNC,
    CrossPointSettings::LIBRARY,
    CrossPointSettings::FILE_TRANSFER,
    CrossPointSettings::CALIBRE_WIRELESS,
    CrossPointSettings::JOIN_NETWORK,
    CrossPointSettings::CREATE_HOTSPOT,
    CrossPointSettings::SCREENSHOT,
    CrossPointSettings::TOGGLE_DARK_MODE,
    CrossPointSettings::FOOTNOTES,
    CrossPointSettings::FILE_BROWSER,
    CrossPointSettings::CREATE_CLIPPING,
    CrossPointSettings::LOOKUP_WORD,
    CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER,
    CrossPointSettings::QUICK_ACTIONS,
    CrossPointSettings::QUICK_LOCK,
    CrossPointSettings::TOGGLE_FRONTLIGHT,
    CrossPointSettings::TOGGLE_TOUCHSCREEN,
    CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES,
    CrossPointSettings::BACK_HOME,
    CrossPointSettings::HOME_READER,
};

inline bool supportsTiltPageTurn() { return halTiltSensor.isAvailable(); }

inline bool isActionAvailable(const uint8_t action) {
  if (action == CrossPointSettings::READING_STATS && !SETTINGS.shouldTrackReadingStats()) return false;
  if (action == CrossPointSettings::PREVIOUS_PAGE || action == CrossPointSettings::NEARBY_POSITION_SYNC ||
      action == CrossPointSettings::LIBRARY || action == CrossPointSettings::SELECT_CHAPTER ||
      action == CrossPointSettings::ROTATE_FLIP)
    return true;
  if (action == CrossPointSettings::QUICK_ACTIONS || action == CrossPointSettings::QUICK_LOCK) return true;
  if (action == CrossPointSettings::TOGGLE_FRONTLIGHT) return Frontlight.present();
  if (action == CrossPointSettings::TOGGLE_TOUCHSCREEN || action == CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES)
    return gpio.hasTouch();
  if (action == CrossPointSettings::HOME_READER || action == CrossPointSettings::BACK_HOME) return true;
  if (action < CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT) {
    return action != CrossPointSettings::TOGGLE_TILT_PAGE_TURN || supportsTiltPageTurn();
  }
  return action == CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER && gpio.hasHomeKey();
}

// Quick Lock needs a single physical shortcut to unlock. Quick Actions opens
// this same menu, so neither belongs in a menu slot.
inline bool isQuickActionSlotActionAvailable(const uint8_t action) {
  return action != CrossPointSettings::QUICK_LOCK && action != CrossPointSettings::QUICK_ACTIONS &&
         isActionAvailable(action);
}

inline StrId actionLabel(const uint8_t action) {
  // Use the directional label for the legacy page-turn action ID.
  if (action == CrossPointSettings::PAGE_TURN) return StrId::STR_NEXT_PAGE;
  if (action < CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT) return actionLabels[action];
  if (action == CrossPointSettings::QUICK_ACTIONS) return StrId::STR_QUICK_ACTIONS;
  if (action == CrossPointSettings::TOGGLE_FRONTLIGHT) return StrId::STR_TOGGLE_FRONTLIGHT;
  if (action == CrossPointSettings::TOGGLE_TOUCHSCREEN) return StrId::STR_TOGGLE_TOUCHSCREEN;
  if (action == CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES) return StrId::STR_DISABLE_PAGE_TURN_GESTURES;
  if (action == CrossPointSettings::QUICK_LOCK) return StrId::STR_QUICK_LOCK;
  if (action == CrossPointSettings::PREVIOUS_PAGE) return StrId::STR_PREV_PAGE;
  if (action == CrossPointSettings::NEARBY_POSITION_SYNC) return StrId::STR_NEARBY_POSITION_SYNC;
  if (action == CrossPointSettings::LIBRARY) return StrId::STR_LIBRARY;
  if (action == CrossPointSettings::HOME_READER) return StrId::STR_HOME_READER;
  if (action == CrossPointSettings::BACK_HOME) return StrId::STR_BACK_HOME;
  if (action == CrossPointSettings::SELECT_CHAPTER) return StrId::STR_SELECT_CHAPTER;
  if (action == CrossPointSettings::ROTATE_FLIP) return StrId::STR_ROTATE_FLIP;
  return StrId::STR_HOME_BUTTON_LOCK;
}

inline void synchronize(CrossPointSettings& settings, Trigger preferred = Trigger::None) {
  const bool shortPower = settings.shortPwrBtn == CrossPointSettings::QUICK_ACTIONS;
  const bool longPower = settings.longPwrBtn == CrossPointSettings::QUICK_ACTIONS;
  const bool longBack = settings.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  const bool longMenu = settings.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  const bool powerUp = settings.powerChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS;
  const bool upDown = settings.sideButtonChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS;
  const bool tapHome = settings.homeButtonTapAction == CrossPointSettings::QUICK_ACTIONS;
  const bool longPressHome = settings.homeButtonLongPressAction == CrossPointSettings::QUICK_ACTIONS;
  const bool doubleTapHome = settings.homeButtonDoubleTapAction == CrossPointSettings::QUICK_ACTIONS;
  const bool sideUpShort = settings.sideButtonUpShort == CrossPointSettings::QUICK_ACTIONS;
  const bool sideUpLong = settings.sideButtonUpLong == CrossPointSettings::QUICK_ACTIONS;
  const bool sideDownShort = settings.sideButtonDownShort == CrossPointSettings::QUICK_ACTIONS;
  const bool sideDownLong = settings.sideButtonDownLong == CrossPointSettings::QUICK_ACTIONS;

  const bool frontLeftLong = settings.frontButtonLeftLong == CrossPointSettings::QUICK_ACTIONS;
  const bool frontRightLong = settings.frontButtonRightLong == CrossPointSettings::QUICK_ACTIONS;
  Trigger owner = preferred;
  if (owner == Trigger::None) {
    if (shortPower)
      owner = Trigger::ShortPower;
    else if (longPower)
      owner = Trigger::LongPower;
    else if (longBack)
      owner = Trigger::LongBack;
    else if (longMenu)
      owner = Trigger::LongMenu;
    else if (powerUp)
      owner = Trigger::PowerUp;
    else if (upDown)
      owner = Trigger::UpDown;
    else if (tapHome)
      owner = Trigger::TapHome;
    else if (longPressHome)
      owner = Trigger::LongPressHome;
    else if (doubleTapHome)
      owner = Trigger::DoubleTapHome;
    else if (sideUpShort)
      owner = Trigger::SideUpShort;
    else if (sideUpLong)
      owner = Trigger::SideUpLong;
    else if (sideDownShort)
      owner = Trigger::SideDownShort;
    else if (sideDownLong)
      owner = Trigger::SideDownLong;
    else if (frontLeftLong)
      owner = Trigger::FrontLeftLong;
    else if (frontRightLong)
      owner = Trigger::FrontRightLong;
  }

  if (owner != Trigger::ShortPower && shortPower) settings.shortPwrBtn = CrossPointSettings::IGNORE;
  if (owner != Trigger::LongPower && longPower) settings.longPwrBtn = CrossPointSettings::IGNORE;
  if (owner != Trigger::LongBack && longBack) settings.longPressBackAction = CrossPointSettings::LONG_MENU_OFF;
  if (owner != Trigger::LongMenu && longMenu) settings.longPressMenuAction = CrossPointSettings::LONG_MENU_OFF;
  if (owner != Trigger::PowerUp && powerUp) settings.powerChordAction = CrossPointSettings::CHORD_DISABLED;
  if (owner != Trigger::UpDown && upDown) settings.sideButtonChordAction = CrossPointSettings::CHORD_DISABLED;
  if (owner != Trigger::TapHome && tapHome) settings.homeButtonTapAction = CrossPointSettings::HOME_BUTTON_BACK_HOME;
  if (owner != Trigger::LongPressHome && longPressHome) {
    settings.homeButtonLongPressAction = CrossPointSettings::HOME_BUTTON_READER_MENU;
  }
  if (owner != Trigger::DoubleTapHome && doubleTapHome) {
    settings.homeButtonDoubleTapAction = CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT;
  }
  if (owner != Trigger::SideUpShort && sideUpShort) settings.sideButtonUpShort = CrossPointSettings::IGNORE;
  if (owner != Trigger::SideUpLong && sideUpLong) settings.sideButtonUpLong = CrossPointSettings::IGNORE;
  if (owner != Trigger::SideDownShort && sideDownShort) settings.sideButtonDownShort = CrossPointSettings::IGNORE;
  if (owner != Trigger::SideDownLong && sideDownLong) settings.sideButtonDownLong = CrossPointSettings::IGNORE;
  if (owner != Trigger::FrontLeftLong && frontLeftLong) settings.frontButtonLeftLong = CrossPointSettings::IGNORE;
  if (owner != Trigger::FrontRightLong && frontRightLong) settings.frontButtonRightLong = CrossPointSettings::IGNORE;
  settings.quickActionsTrigger = static_cast<uint8_t>(owner);
}

inline void applyTrigger(CrossPointSettings& settings, const Trigger trigger) {
  if (settings.frontButtonLeftLong == CrossPointSettings::QUICK_ACTIONS)
    settings.frontButtonLeftLong = CrossPointSettings::IGNORE;
  if (settings.frontButtonRightLong == CrossPointSettings::QUICK_ACTIONS)
    settings.frontButtonRightLong = CrossPointSettings::IGNORE;
  if (settings.shortPwrBtn == CrossPointSettings::QUICK_ACTIONS) settings.shortPwrBtn = CrossPointSettings::IGNORE;
  if (settings.longPwrBtn == CrossPointSettings::QUICK_ACTIONS) settings.longPwrBtn = CrossPointSettings::IGNORE;
  if (settings.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS) {
    settings.longPressBackAction = CrossPointSettings::LONG_MENU_OFF;
  }
  if (settings.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_ACTIONS) {
    settings.longPressMenuAction = CrossPointSettings::LONG_MENU_OFF;
  }
  if (settings.powerChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS) {
    settings.powerChordAction = CrossPointSettings::CHORD_DISABLED;
  }
  if (settings.sideButtonChordAction == CrossPointSettings::CHORD_QUICK_ACTIONS) {
    settings.sideButtonChordAction = CrossPointSettings::CHORD_DISABLED;
  }
  if (settings.homeButtonTapAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonTapAction = CrossPointSettings::HOME_BUTTON_BACK_HOME;
  }
  if (settings.homeButtonLongPressAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonLongPressAction = CrossPointSettings::HOME_BUTTON_READER_MENU;
  }
  if (settings.homeButtonDoubleTapAction == CrossPointSettings::QUICK_ACTIONS) {
    settings.homeButtonDoubleTapAction = CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT;
  }
  if (settings.sideButtonUpShort == CrossPointSettings::QUICK_ACTIONS)
    settings.sideButtonUpShort = CrossPointSettings::IGNORE;
  if (settings.sideButtonUpLong == CrossPointSettings::QUICK_ACTIONS)
    settings.sideButtonUpLong = CrossPointSettings::IGNORE;
  if (settings.sideButtonDownShort == CrossPointSettings::QUICK_ACTIONS)
    settings.sideButtonDownShort = CrossPointSettings::IGNORE;
  if (settings.sideButtonDownLong == CrossPointSettings::QUICK_ACTIONS)
    settings.sideButtonDownLong = CrossPointSettings::IGNORE;

  if (trigger == Trigger::ShortPower) settings.shortPwrBtn = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongPower) settings.longPwrBtn = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongBack) settings.longPressBackAction = CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  if (trigger == Trigger::LongMenu) settings.longPressMenuAction = CrossPointSettings::LONG_MENU_QUICK_ACTIONS;
  if (trigger == Trigger::PowerUp) settings.powerChordAction = CrossPointSettings::CHORD_QUICK_ACTIONS;
  if (trigger == Trigger::UpDown) settings.sideButtonChordAction = CrossPointSettings::CHORD_QUICK_ACTIONS;
  if (trigger == Trigger::TapHome) settings.homeButtonTapAction = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::LongPressHome) settings.homeButtonLongPressAction = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::DoubleTapHome) settings.homeButtonDoubleTapAction = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::SideUpShort) settings.sideButtonUpShort = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::SideUpLong) settings.sideButtonUpLong = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::SideDownShort) settings.sideButtonDownShort = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::SideDownLong) settings.sideButtonDownLong = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::FrontLeftLong) settings.frontButtonLeftLong = CrossPointSettings::QUICK_ACTIONS;
  if (trigger == Trigger::FrontRightLong) settings.frontButtonRightLong = CrossPointSettings::QUICK_ACTIONS;
  settings.quickActionsTrigger = static_cast<uint8_t>(trigger);
}

inline Trigger triggerForSetting(uint8_t CrossPointSettings::* member) {
  if (member == &CrossPointSettings::shortPwrBtn) return Trigger::ShortPower;
  if (member == &CrossPointSettings::longPwrBtn) return Trigger::LongPower;
  if (member == &CrossPointSettings::longPressBackAction) return Trigger::LongBack;
  if (member == &CrossPointSettings::longPressMenuAction) return Trigger::LongMenu;
  if (member == &CrossPointSettings::powerChordAction) return Trigger::PowerUp;
  if (member == &CrossPointSettings::sideButtonChordAction) return Trigger::UpDown;
  if (member == &CrossPointSettings::homeButtonTapAction) return Trigger::TapHome;
  if (member == &CrossPointSettings::homeButtonLongPressAction) return Trigger::LongPressHome;
  if (member == &CrossPointSettings::homeButtonDoubleTapAction) return Trigger::DoubleTapHome;
  if (member == &CrossPointSettings::sideButtonUpShort) return Trigger::SideUpShort;
  if (member == &CrossPointSettings::sideButtonUpLong) return Trigger::SideUpLong;
  if (member == &CrossPointSettings::sideButtonDownShort) return Trigger::SideDownShort;
  if (member == &CrossPointSettings::sideButtonDownLong) return Trigger::SideDownLong;
  if (member == &CrossPointSettings::frontButtonLeftLong) return Trigger::FrontLeftLong;
  if (member == &CrossPointSettings::frontButtonRightLong) return Trigger::FrontRightLong;
  return Trigger::None;
}

inline void settingChanged(CrossPointSettings& settings, uint8_t CrossPointSettings::* member) {
  const Trigger trigger = triggerForSetting(member);
  if (trigger == Trigger::None) return;
  const bool selected = trigger == Trigger::LongBack || trigger == Trigger::LongMenu
                            ? settings.*member == CrossPointSettings::LONG_MENU_QUICK_ACTIONS
                        : (trigger == Trigger::PowerUp || trigger == Trigger::UpDown)
                            ? settings.*member == CrossPointSettings::CHORD_QUICK_ACTIONS
                            : settings.*member == CrossPointSettings::QUICK_ACTIONS;
  synchronize(settings, selected ? trigger : Trigger::None);
}

using ActionHandler = std::function<void(CrossPointSettings::SHORT_PWRBTN)>;
using ActionFilter = std::function<bool(CrossPointSettings::SHORT_PWRBTN)>;

void showConfiguredPopup(OptionPopup& popup, const std::function<void()>& requestUpdate,
                         ActionHandler actionHandler = {}, ActionFilter actionFilter = {});
}  // namespace QuickActions
