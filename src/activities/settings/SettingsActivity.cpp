#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "AboutActivity.h"
#include "AppCapabilities.h"
#include "AppVersion.h"
#include "BackupStatsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceCapabilities.h"
#include "FilenameFontSystem.h"
#include "FontSelectionActivity.h"
#include "HyphenationManagerActivity.h"
#include "util/PluginEvents.h"
#if CROSSINK_SCALABLE_FONTS
#include "TtfRenderOptionsActivity.h"
#endif
#include "AssetDownloadActivity.h"
#include "FrontlightTimePickerActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "QuickActions.h"
#include "QuickActionsActivity.h"
#include "ReleaseNotesActivity.h"
#include "ScreenCalibrationActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/frontlightHeaderIcons.h"
#include "fontIds.h"
#include "util/DictionaryRegistry.h"
#include "util/FrontlightSchedule.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_TAB = 2;
constexpr int16_t TOUCH_TAB_BAR_HEIGHT = 50;
#if CROSSINK_SCALABLE_FONTS
StrId uiFontErrorString(const FontLoadError error) {
  switch (error) {
    case FontLoadError::Memory:
      return StrId::STR_UI_FONT_MEMORY_ERROR;
    case FontLoadError::Unsupported:
      return StrId::STR_UI_FONT_UNSUPPORTED;
    case FontLoadError::TooLarge:
      return StrId::STR_UI_FONT_TOO_LARGE;
    case FontLoadError::InvalidFamily:
    case FontLoadError::NoFont:
      return StrId::STR_UI_FONT_FAMILY_ERROR;
    default:
      return StrId::STR_FONT_DATA_UNREADABLE;
  }
}
#endif
}  // namespace

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

namespace {
constexpr int systemVersionFooterSideMargin = 20;
constexpr int systemVersionFooterBottomInset = 15;
constexpr size_t controlsParentBaseCount = 3;
constexpr size_t controlsHomeButtonCount = 4;
constexpr size_t controlsPowerMinCount = 2;
constexpr size_t controlsPowerMaxCount = 3;
constexpr size_t controlsFrontButtonCount = 7;
constexpr size_t controlsSideButtonBaseCount = 7;

void formatFrontlightScheduleTime(const uint16_t timeOfDay, char* const buf, const size_t len) {
  const FrontlightSchedule::TimeOfDay time = FrontlightSchedule::timeOfDayFromMinutes(timeOfDay);
  snprintf(buf, len, "%u:%02u %s", static_cast<unsigned>(time.hour12), static_cast<unsigned>(time.minute),
           I18N.get(time.isPm ? StrId::STR_PM : StrId::STR_AM));
}

Rect settingsHeaderRect(const GfxRenderer& renderer) { return TouchHeaderBackButton::compactHeaderRect(renderer); }

bool useLandscapeTouchLayout(const GfxRenderer& renderer) {
  // Layout is an app capability decision, not a live GT911 probe or SDK board
  // profile result. The simulator supplies touch through its own device
  // profile, while firmware can construct Settings during touch reinitialization.
#if CROSSINK_APP_CAP_TOUCH
  return renderer.getScreenWidth() > renderer.getScreenHeight();
#else
  return false;
#endif
}

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

void drawCenteredTextLine(const GfxRenderer& renderer, const Rect& safe, const int y, const std::string& text) {
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
  const int labelX = safe.x + (safe.width - labelWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, labelX, y, text.c_str());
}

bool isVersionBreakChar(const char c) { return c == ' ' || c == '-' || c == '+' || c == '.' || c == '_'; }

std::string formatUtcOffset(uint8_t biasedQ) {
  if (biasedQ > 104) biasedQ = 48;
  const int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
  const bool neg = totalMinutes < 0;
  const int absMinutes = neg ? -totalMinutes : totalMinutes;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', absMinutes / 60, absMinutes % 60);
  return buf;
}

std::string formatCompactDuration(const uint32_t seconds) {
  char buf[24];
  if (seconds < 60) {
    snprintf(buf, sizeof(buf), "%lus", static_cast<unsigned long>(seconds));
  } else if (seconds % 60 == 0) {
    snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(buf, sizeof(buf), "%lum %lus", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  }
  return buf;
}

void drawSystemVersionFooter(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const std::string label = AppVersion::versionLabel();
  const int maxWidth = safe.width - systemVersionFooterSideMargin * 2;
  const int bottomLineY = safe.y + safe.height - metrics.verticalSpacing - systemVersionFooterBottomInset;

  if (renderer.getTextWidth(SMALL_FONT_ID, label.c_str()) <= maxWidth) {
    drawCenteredTextLine(renderer, safe, bottomLineY, label);
    return;
  }

  size_t fallbackBreak = std::string::npos;
  size_t preferredBreak = std::string::npos;
  for (size_t i = 1; i < label.size(); i++) {
    if (!isVersionBreakChar(label[i - 1])) continue;

    const std::string firstLine = label.substr(0, i);
    if (renderer.getTextWidth(SMALL_FONT_ID, firstLine.c_str()) > maxWidth) break;

    fallbackBreak = i;
    const std::string secondLine = label.substr(i);
    if (renderer.getTextWidth(SMALL_FONT_ID, secondLine.c_str()) <= maxWidth) {
      preferredBreak = i;
    }
  }

  const size_t lineBreak = preferredBreak != std::string::npos ? preferredBreak : fallbackBreak;
  const std::string firstLine = lineBreak == std::string::npos
                                    ? renderer.truncatedText(SMALL_FONT_ID, label.c_str(), maxWidth)
                                    : label.substr(0, lineBreak);
  const std::string secondLine = lineBreak == std::string::npos
                                     ? ""
                                     : renderer.truncatedText(SMALL_FONT_ID, label.substr(lineBreak).c_str(), maxWidth);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  drawCenteredTextLine(renderer, safe, bottomLineY - lineHeight, firstLine);
  drawCenteredTextLine(renderer, safe, bottomLineY, secondLine);
}

std::string formatSettingValue(const SettingInfo& setting) {
  if (setting.value16Ptr == &CrossPointSettings::frontlightScheduleStart ||
      setting.value16Ptr == &CrossPointSettings::frontlightScheduleEnd) {
    const uint16_t timeOfDay = SETTINGS.*(setting.value16Ptr);
    if (SETTINGS.frontlightScheduleEnabled == 0 || !FrontlightSchedule::isTimeOfDayValid(timeOfDay)) return "--";
    char valueBuffer[16];
    formatFrontlightScheduleTime(timeOfDay, valueBuffer, sizeof(valueBuffer));
    return valueBuffer;
  }
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return tr(STR_NEVER);
    }
    char valueBuffer[32];
    snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
             static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
    return valueBuffer;
  }
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    return std::to_string(SETTINGS.*(setting.valuePtr)) + "%";
  }
  if (setting.valuePtr == &CrossPointSettings::readingIdleTimeThresholdUnits) {
    return formatCompactDuration(SETTINGS.getReadingIdleTimeThresholdSeconds());
  }
  if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
    return formatUtcOffset(SETTINGS.*(setting.valuePtr));
  }
  if (setting.valuePtr == &CrossPointSettings::wordSpacing) {
    return std::to_string(WordSpacing::level(SETTINGS.wordSpacing));
  }
  if (setting.valuePtr == &CrossPointSettings::characterSpacing) {
    char value[8];
    CrossPointSettings::formatCharacterSpacing(SETTINGS.characterSpacing, value, sizeof(value));
    return value;
  }
  return std::to_string(SETTINGS.*(setting.valuePtr));
}

fui::BitmapRef swipeActionIcon(const StrId nameId) {
  switch (nameId) {
    case StrId::STR_LEFT_EDGE_UP:
    case StrId::STR_RIGHT_EDGE_UP:
      return fui::bitmapFromIcon(icon_arrow_up_24);
    case StrId::STR_LEFT_EDGE_DOWN:
    case StrId::STR_RIGHT_EDGE_DOWN:
      return fui::bitmapFromIcon(icon_arrow_down_24);
    case StrId::STR_TWO_FINGER_SWIPE_UP:
      return fui::bitmapFromIcon(icon_arrows_up_24);
    case StrId::STR_TWO_FINGER_SWIPE_DOWN:
      return fui::bitmapFromIcon(icon_arrows_down_24);
    case StrId::STR_TWO_FINGER_SWIPE_LEFT:
      return fui::bitmapFromIcon(icon_arrows_left_24);
    case StrId::STR_TWO_FINGER_SWIPE_RIGHT:
      return fui::bitmapFromIcon(icon_arrows_right_24);
    default:
      return {};
  }
}

fui::BitmapRef frontlightScheduleEndpointIcon(const SettingInfo& setting) {
  if (setting.value16Ptr == &CrossPointSettings::frontlightScheduleStart) {
    return fui::bitmapFromIcon(icon_lightbulb_28);
  }
  if (setting.value16Ptr == &CrossPointSettings::frontlightScheduleEnd) {
    return fui::bitmapFromIcon(icon_lightbulb_off_28);
  }
  return {};
}

bool isTwoFingerSwipeSetting(const uint8_t CrossPointSettings::* const valuePtr) {
  return valuePtr == &CrossPointSettings::twoFingerSwipeUp || valuePtr == &CrossPointSettings::twoFingerSwipeDown ||
         valuePtr == &CrossPointSettings::twoFingerSwipeLeft || valuePtr == &CrossPointSettings::twoFingerSwipeRight;
}

std::string trimAsciiSpaces(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}
}  // namespace

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool dismissOnUpSwipe,
                                   const bool returnToParentOnClose, const View view)
    : Activity(view == View::FileBrowser ? "FileBrowserSettings" : "Settings", renderer, mappedInput),
      dismissOnUpSwipe(dismissOnUpSwipe),
      returnToParentOnClose(returnToParentOnClose),
      view(view),
      entryOrientation(renderer.getOrientation()),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  displaySleepSettings.clear();
  displayFrontlightSettings.clear();
  readerSettings.clear();
  readerFontSettings.clear();
  readerPageLayoutSettings.clear();
  controlsSettings.clear();
  controlsPowerSettings.clear();
  controlsHomeButtonSettings.clear();
  controlsFrontButtonSettings.clear();
  controlsSideButtonSettings.clear();
  controlsTapsGesturesSettings.clear();
  controlsTwoFingerSwipeSettings.clear();
  controlsEdgeGestureSettings.clear();
  systemSettings.clear();
  systemDeviceSettings.clear();
  systemAdvancedSettings.clear();
  systemFilesCacheSettings.clear();
  fileBrowserSettings.clear();
  systemReadingStatsSettings.clear();
  systemGlobalStatsSettings.clear();

  if (isFileBrowserView()) {
    fileBrowserSettings = buildFileBrowserSettingsList(getBaseSettingsList());
    currentSettings = &fileBrowserSettings;
    settingsCount = static_cast<int>(currentSettings->size());
    return;
  }

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  const bool needsFonts = activeSubmenu == SettingAction::ReaderFontOptions;
  if (needsFonts) sdFontSystem.refreshIfDirty();

  dictionaryRegistry.refreshIfDirty();
  const auto allSettings = getSettingsList(needsFonts ? &sdFontSystem.registry() : nullptr, &dictionaryRegistry);
  displaySettings = buildGroupedDisplaySettingsList(allSettings);
#ifndef SIMULATOR
  if (BoardConfig::isX4Pro() || CROSSINK_APP_DEVICE_X4CLASSIC) {
    displaySettings.erase(
        std::remove_if(displaySettings.begin(), displaySettings.end(),
                       [](const SettingInfo& setting) { return setting.valuePtr == &CrossPointSettings::fadingFix; }),
        displaySettings.end());
  }
#endif
  displaySleepSettings = buildDisplaySleepSettingsList(allSettings);
  displayFrontlightSettings = buildDisplayFrontlightSettingsList(allSettings);
  readerSettings = buildReaderSettingsParentList(allSettings);
  readerFontSettings = buildReaderFontSettingsList(allSettings);
#if CROSSINK_SCALABLE_FONTS
  if (needsFonts && sdFontSystem.isScalableFamily(SETTINGS.sdFontFamilyName)) {
    const auto fontSize =
        std::find_if(readerFontSettings.begin(), readerFontSettings.end(),
                     [](const SettingInfo& setting) { return setting.nameId == StrId::STR_FONT_SIZE; });
    const auto insertAt = fontSize == readerFontSettings.end() ? readerFontSettings.end() : std::next(fontSize);
    readerFontSettings.insert(insertAt, SettingInfo::Action(StrId::STR_TTF_RENDERING, SettingAction::TtfRendering));
  }
#endif
  readerPageLayoutSettings = buildReaderPageLayoutSettingsList(allSettings);
  readerScreenMarginSettings = buildReaderScreenMarginSettingsList(allSettings);
  systemSettings = buildSystemSettingsParentList(allSettings);
  systemDeviceSettings = buildSystemDeviceSettingsList(allSettings);
  systemAdvancedSettings = buildSystemAdvancedSettingsList(allSettings);
  systemFilesCacheSettings = buildSystemFilesCacheSettingsList(allSettings);
  systemReadingStatsSettings = buildSystemReadingStatsSettingsList(allSettings);
  systemGlobalStatsSettings = buildSystemGlobalStatsSettingsList(allSettings);
  controlsSettings = buildControlsSettingsParentList(allSettings);
  controlsPowerSettings = buildControlsPowerSettingsList(allSettings);
  controlsHomeButtonSettings = buildControlsHomeButtonSettingsList(allSettings);
  controlsTapsGesturesSettings = buildControlsTapsGesturesSettingsList(allSettings);
  controlsTwoFingerSwipeSettings = buildControlsTwoFingerSwipeSettingsList(allSettings);
  controlsEdgeGestureSettings = buildControlsEdgeGestureSettingsList(allSettings);
  const size_t expectedSideButtonCount =
      controlsSideButtonBaseCount + (hasSideButtonChordSetting(allSettings) ? 1u : 0u);
  const bool hasFrontButtons = deviceHasFrontButtons();
  if (hasFrontButtons) {
    controlsFrontButtonSettings = buildControlsFrontButtonSettingsList(allSettings);
  }
  controlsSideButtonSettings = buildControlsSideButtonSettingsList(allSettings);

  // Front Buttons and Menu Navigation are both absent from touch-device menus.
  const size_t expectedControlsCount = controlsParentBaseCount + SLIDE_SWITCH_SETTINGS_COUNT +
                                       (hasFrontButtons ? 2u : 0u) + (gpio.hasHomeKey() ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN) ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION) ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_TILT_NEXT_PAGE_ONLY) ? 1u : 0u) +
                                       (hasSettingByName(allSettings, StrId::STR_NEXT_PAGE) ? 1u : 0u);
  const size_t expectedFrontButtonCount = hasFrontButtons ? controlsFrontButtonCount : 0u;
  if (controlsSettings.size() != expectedControlsCount ||
      (gpio.hasHomeKey() && controlsHomeButtonSettings.size() != controlsHomeButtonCount) ||
      controlsPowerSettings.size() < controlsPowerMinCount || controlsPowerSettings.size() > controlsPowerMaxCount ||
      controlsFrontButtonSettings.size() != expectedFrontButtonCount ||
      controlsSideButtonSettings.size() != expectedSideButtonCount) {
    LOG_ERR("SET", "Unexpected controls menu counts: controls=%u/%u home=%u power=%u front=%u side=%u",
            static_cast<uint32_t>(controlsSettings.size()), static_cast<uint32_t>(expectedControlsCount),
            static_cast<uint32_t>(controlsHomeButtonSettings.size()),
            static_cast<uint32_t>(controlsPowerSettings.size()),
            static_cast<uint32_t>(controlsFrontButtonSettings.size()),
            static_cast<uint32_t>(controlsSideButtonSettings.size()));
  }

  setCurrentSettingsForCategory();
}

void SettingsActivity::setCurrentSettingsForCategory() {
  if (isFileBrowserView()) {
    currentSettings = &fileBrowserSettings;
    settingsCount = static_cast<int>(currentSettings->size());
    return;
  }

  switch (selectedCategoryIndex) {
    case 0:
      if (activeSubmenu == SettingAction::DisplaySleepScreen) {
        currentSettings = &displaySleepSettings;
      } else if (activeSubmenu == SettingAction::DisplayFrontlight) {
        currentSettings = &displayFrontlightSettings;
      } else {
        currentSettings = &displaySettings;
      }
      break;
    case 1:
      switch (activeSubmenu) {
        case SettingAction::ReaderFontOptions:
          currentSettings = &readerFontSettings;
          break;
        case SettingAction::ReaderPageLayout:
          currentSettings = &readerPageLayoutSettings;
          break;
        case SettingAction::ScreenMargin:
          currentSettings = &readerScreenMarginSettings;
          break;
        default:
          currentSettings = &readerSettings;
          break;
      }
      break;
    case 2:
      switch (activeSubmenu) {
        case SettingAction::ControlsPowerButton:
          currentSettings = &controlsPowerSettings;
          break;
        case SettingAction::ControlsHomeButton:
          currentSettings = &controlsHomeButtonSettings;
          break;
        case SettingAction::ControlsFrontButtons:
          currentSettings = &controlsFrontButtonSettings;
          break;
        case SettingAction::ControlsSideButtons:
          currentSettings = &controlsSideButtonSettings;
          break;
        case SettingAction::ControlsTapsGestures:
          currentSettings = &controlsTapsGesturesSettings;
          break;
        case SettingAction::ControlsTwoFingerSwipe:
          currentSettings = &controlsTwoFingerSwipeSettings;
          break;
        case SettingAction::ControlsEdgeGestures:
          currentSettings = &controlsEdgeGestureSettings;
          break;
        default:
          currentSettings = &controlsSettings;
          break;
      }
      break;
    case 3:
      switch (activeSubmenu) {
        case SettingAction::SystemDevice:
          currentSettings = &systemDeviceSettings;
          break;
        case SettingAction::SystemAdvanced:
          currentSettings = &systemAdvancedSettings;
          break;
        case SettingAction::SystemFilesCache:
          currentSettings = &systemFilesCacheSettings;
          break;
        case SettingAction::SystemReadingStats:
          currentSettings = &systemReadingStatsSettings;
          break;
        case SettingAction::SystemGlobalStats:
          currentSettings = &systemGlobalStatsSettings;
          break;
        default:
          currentSettings = &systemSettings;
          break;
      }
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::enterCategory(int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  activeSubmenu = SettingAction::None;
  parentSubmenu = SettingAction::None;
  setCurrentSettingsForCategory();
  showSettingSelection = true;
}

StrId SettingsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::DisplaySleepScreen:
      return StrId::STR_SLEEP_SCREEN;
    case SettingAction::DisplayFrontlight:
      return StrId::STR_FRONTLIGHT;
    case SettingAction::ReaderFontOptions:
      return StrId::STR_READER_FONT_OPTIONS;
    case SettingAction::ReaderPageLayout:
      return StrId::STR_READER_PAGE_LAYOUT;
    case SettingAction::ScreenMargin:
      return StrId::STR_SCREEN_MARGIN;
    case SettingAction::ControlsPowerButton:
      return StrId::STR_POWER_BUTTON;
    case SettingAction::ControlsHomeButton:
      return StrId::STR_HOME_BUTTON;
    case SettingAction::ControlsFrontButtons:
      return StrId::STR_FRONT_BUTTONS;
    case SettingAction::ControlsSideButtons:
      return StrId::STR_SIDE_BUTTONS;
    case SettingAction::ControlsTapsGestures:
      return StrId::STR_TAPS_AND_GESTURES;
    case SettingAction::ControlsTwoFingerSwipe:
      return StrId::STR_TWO_FINGER_SWIPE;
    case SettingAction::ControlsEdgeGestures:
      return StrId::STR_EDGE_GESTURES;
    case SettingAction::SystemDevice:
      return StrId::STR_SYSTEM_DEVICE;
    case SettingAction::SystemAdvanced:
      return StrId::STR_SYSTEM_ADVANCED;
    case SettingAction::SystemFilesCache:
      return StrId::STR_SYSTEM_FILES_CACHE;
    case SettingAction::SystemReadingStats:
      return StrId::STR_READING_STATS;
    case SettingAction::SystemGlobalStats:
      return StrId::STR_ALL_TIME_STATS;
    default:
      return StrId::STR_NONE_OPT;
  }
}

void SettingsActivity::openSubmenu(SettingAction action) {
  parentSubmenu = activeSubmenu;
  activeSubmenu = action;
  if (action == SettingAction::ReaderFontOptions) {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_LOADING), true);
    rebuildSettingsLists();
  }
  setCurrentSettingsForCategory();
  selectedSettingIndex = 1;
  showSettingSelection = true;
  while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
         (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
  }
}

void SettingsActivity::closeSubmenu() {
  const SettingAction closedSubmenu = activeSubmenu;
  activeSubmenu = parentSubmenu;
  parentSubmenu = SettingAction::None;
  setCurrentSettingsForCategory();
  showSettingSelection = true;

  // Return the highlight to the row that opened the submenu.
  selectedSettingIndex = 1;
  for (int index = 0; index < settingsCount; ++index) {
    const SettingInfo& setting = (*currentSettings)[index];
    if (setting.type == SettingType::SUBMENU && setting.action == closedSubmenu) {
      selectedSettingIndex = index + 1;
      break;
    }
  }
  topIndex = followListSelection(selectedSettingIndex - 1, topIndex, visibleRows, settingsCount);
}

bool SettingsActivity::currentSettingUsesOptionMenu(const SettingInfo& setting) const {
  return setting.nameId != StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM &&
         (settingEnumOptionCount(setting) > 2 || setting.valuePtr == &CrossPointSettings::menuNavigation) &&
         (setting.valuePtr != nullptr || (setting.valueGetter && setting.valueSetter));
}

void SettingsActivity::openEnumOptionPicker(const SettingInfo& setting) {
  const size_t optionCount = settingEnumOptionCount(setting);
  if (optionCount == 0) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (uint8_t i = 0; i < optionCount; i++) {
    options.push_back(sideButtonOptionLabel(setting, i));
  }

  uint8_t currentIndex = 0;
  if (setting.valuePtr != nullptr) {
    currentIndex = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
  } else if (setting.valueGetter) {
    currentIndex = setting.valueGetter();
  }
  if (currentIndex >= optionCount) currentIndex = 0;

  const SettingInfo selectedSetting = setting;
  OptionPopup::Note note;
  if (setting.valuePtr == &CrossPointSettings::wifiTransmitPower) {
    note = {tr(STR_NOTE), tr(STR_WIFI_TRANSMIT_POWER_HELP)};
  } else if (setting.valuePtr == &CrossPointSettings::sideButtonChordAction && mappedInput.hasTouchHardware()) {
    note = {tr(STR_NOTE), tr(STR_TOUCHSCREEN_ESCAPE_HATCH_NOTE)};
  }
  optionPopup.show(
      setting.nameId, options, currentIndex,
      [this, selectedSetting](int selectedIndex) {
        if (selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) =
              enumRawValueForDisplayIndex(selectedSetting, static_cast<uint8_t>(selectedIndex));
          if (isTwoFingerSwipeSetting(selectedSetting.valuePtr)) {
            CrossPointSettings::normalizeTwoFingerSwipeActions(SETTINGS, selectedSetting.valuePtr);
          }
          QuickActions::settingChanged(SETTINGS, selectedSetting.valuePtr);
        } else if (selectedSetting.valueSetter) {
          selectedSetting.valueSetter(static_cast<uint8_t>(selectedIndex));
        }

        const bool sleepScreenChanged = selectedSetting.valuePtr == &CrossPointSettings::sleepScreen;
        const bool quickResumeTimeoutChanged = selectedSetting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
        requestUpdate();
      },
      note);
  if (setting.valuePtr == &CrossPointSettings::menuNavigation) {
    menuNavigationNote.apply(optionPopup);
  }
  requestUpdate();
}

void SettingsActivity::openScreenMarginPicker(const SettingInfo& setting) {
  if (setting.valuePtr == nullptr) return;

  const SettingInfo selectedSetting = setting;
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "ScreenMarginInterval", selectedSetting.nameId, SETTINGS.*(selectedSetting.valuePtr),
          CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN,
          CrossPointSettings::SCREEN_MARGIN_SMALL_STEP, CrossPointSettings::SCREEN_MARGIN_LARGE_STEP,
          StrId::STR_NONE_OPT, /*readerActivity=*/false,
          /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/false,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/5, /*useReaderSlider=*/true,
          selectedSetting.valuePtr == &CrossPointSettings::screenMarginVertical
              ? IntervalSelectionActivity::ReaderPreviewSetting::VerticalMargin
              : IntervalSelectionActivity::ReaderPreviewSetting::HorizontalMargin),
      [this, selectedSetting](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.*(selectedSetting.valuePtr) = static_cast<uint8_t>(std::clamp(
              std::get<IntervalResult>(result.data).value, static_cast<uint32_t>(CrossPointSettings::MIN_SCREEN_MARGIN),
              static_cast<uint32_t>(CrossPointSettings::MAX_SCREEN_MARGIN)));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openWordSpacingPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "WordSpacingInterval", StrId::STR_WORD_SPACING,
          WordSpacing::sliderValue(SETTINGS.wordSpacing), 0, CrossPointSettings::MAX_WORD_SPACING, 1, 1,
          StrId::STR_NONE_OPT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true, /*valueFormatter=*/CrossPointSettings::formatWordSpacingSlider,
          /*tapStep=*/1,
          /*useReaderSlider=*/true, IntervalSelectionActivity::ReaderPreviewSetting::WordSpacing),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.wordSpacing = WordSpacing::fromSlider(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openCharacterSpacingPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "CharacterSpacingInterval", StrId::STR_CHARACTER_SPACING, SETTINGS.characterSpacing, 0,
          CrossPointSettings::MAX_CHARACTER_SPACING, 1, 1, StrId::STR_NONE_OPT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true, /*valueFormatter=*/CrossPointSettings::formatCharacterSpacing,
          /*tapStep=*/1,
          /*useReaderSlider=*/true, IntervalSelectionActivity::ReaderPreviewSetting::CharacterSpacing),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.characterSpacing =
              static_cast<uint8_t>(std::clamp(std::get<IntervalResult>(result.data).value, static_cast<uint32_t>(0),
                                              static_cast<uint32_t>(CrossPointSettings::MAX_CHARACTER_SPACING)));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openFilenameFontPicker() {
#if CROSSINK_SCALABLE_FONTS
  RenderLock lock(*this);
  optionPopup.clear();
  if (!filenameFontSystem.discover(filenameFontNames)) {
    optionPopup.show(
        StrId::STR_FILENAME_FALLBACK_FONT, {tr(STR_OK)}, 0, [this](int) { requestUpdate(); },
        OptionPopup::Note(I18N.get(uiFontErrorString(filenameFontSystem.lastError())), tr(STR_FILENAME_FONT_HINT)));
    requestUpdate();
    return;
  }
  filenameFontNames.insert(filenameFontNames.begin(), {tr(STR_NONE_OPT), FontLoadError::None});
  int selected = 0;
  for (size_t i = 1; i < filenameFontNames.size(); ++i) {
    if (filenameFontNames[i].name == SETTINGS.filenameFallbackFont) selected = static_cast<int>(i);
  }
  optionPopup.showBorrowed(
      StrId::STR_FILENAME_FALLBACK_FONT,
      OptionLabels(&filenameFontNames, filenameFontNames.size(),
                   [](const void* owner, size_t i) {
                     return (*static_cast<const std::vector<FilenameFontSystem::Family>*>(owner))[i].name.c_str();
                   }),
      selected,
      [this](int index) {
        if (index < 0 || static_cast<size_t>(index) >= filenameFontNames.size()) return;
        std::strncpy(pendingFilenameFont, index ? filenameFontNames[index].name.c_str() : "",
                     sizeof(pendingFilenameFont) - 1);
        filenameFontSelectionPending = true;
      },
      OptionPopup::Note("", tr(STR_FILENAME_FONT_HINT)));
  // At most 65 borrowed-text notes (~2 KiB), allocated once on this cold path
  // and owned/reused by the popup; too large for the render-task stack.
  std::vector<OptionPopup::Note> notes;
  notes.reserve(filenameFontNames.size());
  for (const auto& family : filenameFontNames) {
    notes.emplace_back(family.error == FontLoadError::None ? "" : I18N.get(uiFontErrorString(family.error)),
                       family.error == FontLoadError::None ? tr(STR_FILENAME_FONT_HINT) : "");
  }
  optionPopup.setOptionNotes(std::move(notes));
  requestUpdate();
#endif
}

void SettingsActivity::openLanguagePicker() {
  RenderLock lock(*this);
  optionPopup.clear();
  languageCatalog.reset();
  // Fixed <3 KiB catalog, cold-path only. A failed allocation still offers
  // English and the active cache without constructing any owning label list.
  languageCatalog.init(MemoryPool::None);
  const auto status = languageCatalog ? I18N.discover(*languageCatalog.get()) : language_cache::Result::Memory;
  const bool cached = std::strcmp(I18N.getCode(), "EN") != 0;
  int currentIndex = cached ? 1 : 0;
  OptionLabels labels;
  bool duplicates = false;
  if (status == language_cache::Result::Ok) {
    for (size_t i = 0; i < languageCatalog->count; ++i) {
      if (std::strcmp(languageCatalog->code(i), I18N.getCode()) == 0) currentIndex = static_cast<int>(i);
      duplicates = duplicates || languageCatalog->disabled(i);
    }
    labels = OptionLabels(
        languageCatalog.get(), languageCatalog->count,
        [](const void* owner, size_t i) { return static_cast<const I18n::Catalog*>(owner)->name(i); },
        [](const void* owner, size_t i) { return static_cast<const I18n::Catalog*>(owner)->disabled(i); });
  } else {
    LOG_ERR("LANG", "Language picker: %s", language_cache::resultName(status));
    languageCatalog.reset();
    labels =
        OptionLabels(nullptr, cached ? 2 : 1, [](const void*, size_t i) { return i ? I18N.getName() : "English"; });
  }
  const char* note = status == language_cache::Result::Ok
                         ? (duplicates ? tr(STR_LANGUAGE_DUPLICATE) : tr(STR_LANGUAGE_APPLY_HINT))
                     : status == language_cache::Result::Memory   ? tr(STR_MEMORY_ERROR)
                     : status == language_cache::Result::TooLarge ? tr(STR_LANGUAGE_CATALOG_LIMIT)
                                                                  : tr(STR_LANGUAGE_IO_ERROR);
  optionPopup.showBorrowed(
      StrId::STR_LANGUAGE, labels, currentIndex,
      [this](int index) {
        if (index < 0) return;
        if (languageCatalog) {
          if (static_cast<size_t>(index) >= languageCatalog->count || languageCatalog->disabled(index)) return;
          languageCatalog->select(index, pendingLanguage);
        } else {
          std::strcpy(pendingLanguage.code, index ? I18N.getCode() : "EN");
          pendingLanguage.path[0] = '\0';
          pendingLanguage.generation = index ? I18N.getGeneration() : 0;
        }
        // Installation starts on the next loop, after this callback returns.
      },
      OptionPopup::Note(tr(STR_LANGUAGE), note));
  requestUpdate();
}

int SettingsActivity::languageSourceIndex(const char* code) const {
  for (size_t i = 0; languageCatalog && i < languageCatalog->count; ++i)
    if (!std::strcmp(languageCatalog->code(i), code)) return static_cast<int>(i);
  return -1;
}
void SettingsActivity::openLanguageDownloads() {
  RenderLock lock(*this);
  optionPopup.clear();
  languageCatalog.reset();
  // Reuse the same bounded offline catalog as Language to tell installed languages
  // from ones to install; online TLS/catalog RAM is acquired only after the
  // network restart that an Install row triggers.
  languageCatalog.init(MemoryPool::None);
  const auto status = languageCatalog ? I18N.discover(*languageCatalog.get()) : language_cache::Result::Memory;
  if (status != language_cache::Result::Ok) {
    LOG_ERR("LANG", "Offline Downloads catalog: %s", language_cache::resultName(status));
    languageCatalog.reset();
    languageError = status == language_cache::Result::Memory ? StrId::STR_MEMORY_ERROR : StrId::STR_LANGUAGE_IO_ERROR;
    return;
  }
  const int currentIndex = static_cast<int>(I18n::languageFromCode(I18N.getCode()));
  languageDownloadsOpen = true;
  std::snprintf(languageRuntimeNote, sizeof(languageRuntimeNote), "%s %s / %llx", tr(STR_CURRENT_VERSION),
                I18N.getCode(), static_cast<unsigned long long>(I18N.getGeneration()));
  // Rows are the firmware's own language table; the shared scratch label is
  // rebuilt per row, so no per-language storage is kept.
  optionPopup.showBorrowed(
      StrId::STR_DOWNLOAD_LANGUAGES,
      OptionLabels(
          this, getLanguageCount(),
          [](const void* owner, size_t index) {
            const auto* self = static_cast<const SettingsActivity*>(owner);
            const char* code = LANGUAGE_CODES[index];
            const char* status = index == 0                             ? tr(STR_HYPHENATION_BUILT_IN)
                                 : !std::strcmp(code, I18N.getCode())   ? tr(STR_ASSET_CURRENT)
                                 : self->languageSourceIndex(code) >= 0 ? tr(STR_INSTALLED)
                                                                        : tr(STR_HYPHENATION_INSTALL_ACTION);
            std::snprintf(self->languageDownloadLabel, sizeof(self->languageDownloadLabel), "%s (%s): %s",
                          I18N.getLanguageName(static_cast<Language>(index)), code, status);
            return static_cast<const char*>(self->languageDownloadLabel);
          },
          [](const void* owner, size_t index) {
            const auto* self = static_cast<const SettingsActivity*>(owner);
            const int source = self->languageSourceIndex(LANGUAGE_CODES[index]);
            return source >= 0 && self->languageCatalog->disabled(source);
          }),
      currentIndex,
      [this](int index) {
        languageDownloadsOpen = false;
        if (index > 0 && index < static_cast<int>(getLanguageCount())) {
          languageDownloadRow = index;
          pendingLanguageDownloadAction = true;
        }
      },
      OptionPopup::Note(languageRuntimeNote, tr(STR_LANGUAGE_APPLY_HINT)));
  optionPopup.setInertOptionIndex(0);
  optionPopup.setCancelCallback([this] { languageDownloadsOpen = false; });
  requestUpdate();
}
void SettingsActivity::startLanguageDownload() {
  {
    RenderLock lock(*this);
    optionPopup.clear();
    languageCatalog.reset();
  }
  APP_STATE.pendingOverlayResume.clear();
  if (APP_STATE.saveToFile() && SETTINGS.saveToFile()) {
    RenderLock restartLock(*this);
    silentRestartToNetwork(NetworkBootTarget::DOWNLOAD_ASSETS,
                           AssetDownloadActivity::bootPayload(AssetCatalog::Kind::Translation, languageDownloadRow));
  } else {
    LOG_ERR("ASSET", "Cannot save state before network restart");
    languageError = StrId::STR_LANGUAGE_SAVE_FAILED;
  }
}
void SettingsActivity::selectLanguageDownload() {
  const char* code = LANGUAGE_CODES[languageDownloadRow];
  const int source = languageSourceIndex(code);
  if (source < 0) {
    startLanguageDownload();  // Not installed: download and install in one step.
    return;
  }
  const bool active = !std::strcmp(code, I18N.getCode());
  RenderLock lock(*this);
  const char* choices[] = {tr(STR_ASSET_USE), tr(STR_UPDATE), tr(STR_CANCEL)};
  const int first = active ? 1 : 0;  // The active language only offers Update / Cancel.
  optionPopup.show(
      I18N.getLanguageName(static_cast<Language>(languageDownloadRow)), choices + first, 3 - first, 2 - first,
      [this, first](int choice) {
        choice += first;
        if (choice == 0)
          languageCatalog->select(languageSourceIndex(LANGUAGE_CODES[languageDownloadRow]), pendingLanguage);
        else if (choice == 1)
          pendingLanguageDownloadConnect = true;
        else
          pendingLanguageDownloads = true;
      },
      OptionPopup::Note(languageRuntimeNote, ""));
  optionPopup.setCancelCallback([this] { pendingLanguageDownloads = true; });
  requestUpdate();
}

void SettingsActivity::applyLanguage(const I18n::Option& selected) {
  if (languageRestartRequired) {
    languageError = StrId::STR_HYPHENATION_APPLY_HINT;
    return;
  }
  // Rendering also reads popup labels; retire them under its existing lock.
  {
    RenderLock lock(*this);
    optionPopup.clear();
    languageCatalog.reset();
  }
  language_cache::Installed installed;
  std::strcpy(installed.metadata.code, selected.code);
  installed.generation = selected.generation;
  {
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LOADING));
    if (selected.path[0]) {
      const auto status = I18N.prepare(selected.path, installed);
      if (status == language_cache::Result::Unchanged) {
        requestUpdate();
        return;
      }
      if (status != language_cache::Result::Ok) {
        if (status == language_cache::Result::RestartRequired) {
          languageRestartRequired = true;
          languageError = StrId::STR_HYPHENATION_APPLY_HINT;
          return;
        }
        languageError = status == language_cache::Result::StorageUnavailable ? StrId::STR_LANGUAGE_STORAGE_UNAVAILABLE
                        : status == language_cache::Result::Memory           ? StrId::STR_MEMORY_ERROR
                        : status == language_cache::Result::Io               ? StrId::STR_LANGUAGE_IO_ERROR
                                                                             : StrId::STR_LANGUAGE_FILE_INVALID;
        return;
      }
      languageRestartRequired = true;
    }
    // The active mapping and every existing string pointer remain untouched.
    // Only a successful settings save selects this generation on the next boot.
    char previousCode[sizeof(SETTINGS.languageCode)];
    std::strcpy(previousCode, SETTINGS.languageCode);
    const uint64_t previousGeneration = SETTINGS.languageCacheGeneration;
    APP_STATE.pendingOverlayResume.clear();
    if (!APP_STATE.saveToFile()) {
      languageError = StrId::STR_LANGUAGE_SAVE_FAILED;
      return;
    }
    std::strcpy(SETTINGS.languageCode, installed.metadata.code);
    SETTINGS.languageCacheGeneration = installed.generation;
    if (!SETTINGS.saveToFile()) {
      std::strcpy(SETTINGS.languageCode, previousCode);
      SETTINGS.languageCacheGeneration = previousGeneration;
      languageError = StrId::STR_LANGUAGE_SAVE_FAILED;
      return;
    }
    silentRestart();
  }
}

void SettingsActivity::openStringEditor(const SettingInfo& setting) {
  std::string initialText;
  if (setting.stringGetter) {
    initialText = setting.stringGetter();
  } else if (setting.stringMaxLen > 0) {
    initialText = reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset;
  }
  if (setting.nameId == StrId::STR_DEVICE_NAME && initialText.empty()) {
    initialText = SETTINGS.getEffectiveDeviceName();
  }

  const size_t maxLength = setting.stringMaxLen > 0 ? setting.stringMaxLen - 1 : 0;
  const size_t minLength = setting.nameId == StrId::STR_DEVICE_NAME ? CrossPointSettings::MIN_DEVICE_NAME_LENGTH : 0;
  const SettingInfo selectedSetting = setting;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(setting.nameId),
                                                                 initialText, maxLength, InputType::Text, minLength),
                         [this, selectedSetting](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }

                           const auto* kb = std::get_if<KeyboardResult>(&result.data);
                           if (kb == nullptr) {
                             requestUpdate();
                             return;
                           }

                           const std::string value = trimAsciiSpaces(kb->text);
                           if (selectedSetting.nameId == StrId::STR_DEVICE_NAME &&
                               (value.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                value.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
                             requestUpdate();
                             return;
                           }

                           if (selectedSetting.stringSetter) {
                             selectedSetting.stringSetter(value);
                           } else if (selectedSetting.stringMaxLen > 0) {
                             char* ptr = reinterpret_cast<char*>(&SETTINGS) + selectedSetting.stringOffset;
                             strncpy(ptr, value.c_str(), selectedSetting.stringMaxLen - 1);
                             ptr[selectedSetting.stringMaxLen - 1] = '\0';
                           }
                           SETTINGS.saveToFile();
                           requestUpdate();
                         });
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  if (touchscreenRecovery) mappedInput.setTouchscreenRecoveryOverride(true);

  // Reapply the orientation captured before the activity being replaced (for
  // example, a landscape reader) performs its normal portrait reset.
  renderer.setOrientation(entryOrientation);
  app.setDevice(uiTarget.deviceContext());

  if (!isFileBrowserView()) {
    // Dictionary names and paths are needed only while the full settings tree
    // is open. Keep the catalog out of the lightweight File Browser editor.
    dictionaryRegistry.discover();
  }

  selectedCategoryIndex = isFileBrowserView() ? 3 : 0;
  selectedSettingIndex = 0;
  activeSubmenu = SettingAction::None;
  parentSubmenu = SettingAction::None;
  if (!isFileBrowserView()) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
    syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);
  }

  rebuildSettingsLists();

  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &SettingsActivity::onRowEvent, this);
  app.on(ACTION_TAB, &SettingsActivity::onTabEvent, this);
  app.setScreen(&SettingsActivity::settingsScreen, this);

  if (openLanguageDownloadsPending && !isFileBrowserView()) {
    // Land on System > Device, then show the Download Languages popup from loop().
    openLanguageDownloadsPending = false;
    enterCategory(categoryCount - 1);
    openSubmenu(SettingAction::SystemDevice);
    pendingLanguageDownloads = true;
  }

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<SettingsActivity*>(user);
  if (self->optionPopup.isActive()) return;
  if (event.value < 0 || event.value >= categoryCount) return;
  self->selectedSettingIndex = 0;
  self->enterCategory(event.value);
  self->topIndex = 0;
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  self->app.clearTapFlash();
}

void SettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<SettingsActivity*>(user);
  if (self->optionPopup.isActive()) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->settingsCount)) return;
  if ((*self->currentSettings)[event.value].type == SettingType::SECTION_HEADER) return;
  self->selectedSettingIndex = event.value + 1;
  if (self->isFileBrowserView()) self->showSettingSelection = false;
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  self->app.clearTapFlash();
  self->toggleCurrentSetting();
}

void SettingsActivity::onExit() {
  if (touchscreenRecovery) mappedInput.setTouchscreenRecoveryOverride(false);
  if (!isFileBrowserView()) {
    dictionaryRegistry.clear();
    sdFontSystem.releaseRegistry();
  }
  // Child Settings returns to the reader's panel; ordinary Settings still
  // returns Home in its usual portrait orientation.
  renderer.setOrientation(returnToParentOnClose ? entryOrientation : GfxRenderer::Orientation::Portrait);
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::closeRootSettings() {
  SETTINGS.saveToFile();
  if (returnToParentOnClose) {
    finishToParent();
  } else {
    onGoHome();
  }
}

void SettingsActivity::finishToParent() {
  setResult(TtfRenderOptionsResult{ttfRenderingChanged});
  finish();
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme and UI-scale changes take effect immediately, on this screen —
  // reload the theme and re-derive the app's fonts and tokens so the very
  // next repaint is in the new look.
  const bool themeChanged = valuePtr == &CrossPointSettings::uiTheme;
  const bool scaleChanged = valuePtr == &CrossPointSettings::uiScale;
  if (!themeChanged && !scaleChanged) {
    return;
  }

  // Rendering reads the shared theme tokens field-by-field. Hold the same
  // mutex used by the render task while replacing the theme and fonts, then
  // release it before requestUpdate() triggers the next repaint.
  {
    RenderLock lock(*this);
    if (themeChanged) {
      UITheme::getInstance().reload();
    }
    const auto spec = uiScaleSpec();
    uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
    uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, spec.titleFontId);
    applySharedUiTheme(app, uiTarget);
  }
}

void SettingsActivity::loop() {
#if CROSSINK_SCALABLE_FONTS
  if (filenameFontSelectionPending) {
    filenameFontSelectionPending = false;
    RenderLock lock(*this);
    char previous[sizeof(SETTINGS.filenameFallbackFont)];
    std::strcpy(previous, SETTINGS.filenameFallbackFont);
    std::strcpy(SETTINGS.filenameFallbackFont, pendingFilenameFont);
    filenameFontSystem.invalidate();
    if (pendingFilenameFont[0]) GUI.drawPopup(renderer, tr(STR_LOADING), true);
    const bool loaded = filenameFontSystem.ensureLoaded(renderer);
    if (loaded && SETTINGS.saveToFile()) {
      // The selected font and persisted setting are ready together.
    } else {
      // Restoring the previous family resets the loader's last failure.
      const StrId error = loaded ? StrId::STR_UI_FONT_SAVE_ERROR : uiFontErrorString(filenameFontSystem.lastError());
      std::strcpy(SETTINGS.filenameFallbackFont, previous);
      filenameFontSystem.ensureLoaded(renderer);
      optionPopup.show(
          StrId::STR_FILENAME_FALLBACK_FONT, {tr(STR_OK)}, 0, [this](int) { requestUpdate(); },
          OptionPopup::Note(I18N.get(error), ""));
    }
    requestUpdate();
    return;
  }
#endif
  if (pendingLanguageDownloads) {
    pendingLanguageDownloads = false;
    openLanguageDownloads();
    return;
  }
  if (pendingLanguageDownloadAction) {
    pendingLanguageDownloadAction = false;
    selectLanguageDownload();
    return;
  }
  if (pendingLanguageDownloadConnect) {
    pendingLanguageDownloadConnect = false;
    startLanguageDownload();
    return;
  }
  if (pendingLanguage.code[0]) {
    const auto selected = pendingLanguage;
    pendingLanguage.code[0] = '\0';
    applyLanguage(selected);
    return;
  }
  // Defer replacing the popup callback until its previous invocation has returned.
  if (languageError != StrId::_COUNT) {
    const StrId error = languageError;
    languageError = StrId::_COUNT;
    RenderLock lock(*this);
    const StrId options[] = {languageRestartRequired ? StrId::STR_CANCEL : StrId::STR_OK, StrId::STR_OK};
    optionPopup.show(
        languageRestartRequired ? error : StrId::STR_LANGUAGE, options, languageRestartRequired ? 2 : 1, 0,
        [this](int choice) {
          if (languageRestartRequired && choice == 1) {
            RenderLock restartLock(*this);
            silentRestart();
          } else
            requestUpdate();
        },
        languageRestartRequired ? OptionPopup::Note(tr(STR_ASSET_USE), tr(STR_HYPHENATION_APPLY_HINT))
                                : OptionPopup::Note(tr(STR_LANGUAGE_INSTALL_FAILED), I18N.get(error)));
    requestUpdate();
    return;
  }
  if (languageDownloadsOpen) {
    // This popup's callbacks only queue work. Serialize its shared label scratch
    // with render/getLayout, without imposing a lock on other Settings callbacks.
    RenderLock lock(*this);
    optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (TouchHeaderBackButton::wasTapped(mappedInput, settingsHeaderRect(renderer))) {
    if (!isFileBrowserView() && activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
    } else {
      closeRootSettings();
    }
    return;
  }

  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showSettingSelection = true;
    if (!isFileBrowserView() && selectedSettingIndex == 0) {
      enterCategory((selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0);
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isFileBrowserView()) {
      closeRootSettings();
      return;
    }
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      showSettingSelection = true;
      requestUpdate();
    } else {
      closeRootSettings();
    }
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the tab and row
  // hit rects; route the snapshot and let the handlers dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint here: it would cost a second e-ink refresh
      // per tap and paint a transient double pill on tab switches whose
      // erased black leaves a partial-refresh ghost.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onTabEvent/onRowEvent
    }
  }

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
#if CROSSINK_APP_CAP_TOUCH
  // Settings opened from the frontlight panel keeps its quick exit, but only
  // from the lower-edge gesture band. Ordinary upward swipes scroll the list
  // in both portrait and landscape.
  const bool dismissFromBottomEdge =
      dismissOnUpSwipe && swipe == MappedInputManager::SwipeDir::Up && mappedInput.wasBottomEdgeUpSwipe();
  if (dismissFromBottomEdge) {
#else
  if (dismissOnUpSwipe && swipe == MappedInputManager::SwipeDir::Up) {
#endif
    SETTINGS.saveToFile();
    finishToParent();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, settingsCount);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  // Buttons walk the tab band (index 0) plus the rows (1..settingsCount).
  const auto moveSelection = [this](int index, const bool forward) {
    while (index > 0 && index <= settingsCount && (*currentSettings)[index - 1].type == SettingType::SECTION_HEADER) {
      index = forward ? ButtonNavigator::nextIndex(index, settingsCount + 1)
                      : ButtonNavigator::previousIndex(index, settingsCount + 1);
    }
    selectedSettingIndex = index;
    showSettingSelection = true;
    if (selectedSettingIndex == 0) {
      topIndex = 0;
    } else {
      topIndex = followListSelection(selectedSettingIndex - 1, topIndex, visibleRows, settingsCount);
    }
    requestUpdate();
  };
  const auto up = mappedInput.menuButton(MappedInputManager::Button::Up);
  const auto down = mappedInput.menuButton(MappedInputManager::Button::Down);
  const auto navigateRows = [this, &moveSelection](const bool forward) {
    const int index = isFileBrowserView()
                          ? (forward ? (selectedSettingIndex >= settingsCount ? 1 : selectedSettingIndex + 1)
                                     : (selectedSettingIndex <= 1 ? settingsCount : selectedSettingIndex - 1))
                          : (forward ? ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1)
                                     : ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1));
    moveSelection(index, forward);
  };
  const bool classicNavigation = SETTINGS.menuNavigation == CrossPointSettings::MENU_NAV_CLASSIC;
  const auto previousButtons =
      (isFileBrowserView() || classicNavigation) ? ButtonNavigator::getPreviousButtons() : std::array{up, up};
  const auto nextButtons =
      (isFileBrowserView() || classicNavigation) ? ButtonNavigator::getNextButtons() : std::array{down, down};
  buttonNavigator.onRelease(nextButtons, [&] { navigateRows(true); });
  buttonNavigator.onRelease(previousButtons, [&] { navigateRows(false); });

  if (!isFileBrowserView()) {
    const auto changeCategory = [this, &hasChangedCategory](const bool forward) {
      hasChangedCategory = true;
      showSettingSelection = true;
      enterCategory(forward ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                            : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount));
      requestUpdate();
    };
    if (classicNavigation) {
      buttonNavigator.onNextContinuous([&] { changeCategory(true); });
      buttonNavigator.onPreviousContinuous([&] { changeCategory(false); });
    } else {
      buttonNavigator.onContinuous(nextButtons, [&] { navigateRows(true); });
      buttonNavigator.onContinuous(previousButtons, [&] { navigateRows(false); });
      const auto left = mappedInput.menuButton(MappedInputManager::Button::Left);
      const auto right = mappedInput.menuButton(MappedInputManager::Button::Right);
      buttonNavigator.onRelease({right, right}, [&] { changeCategory(true); });
      buttonNavigator.onRelease({left, left}, [&] { changeCategory(false); });
      buttonNavigator.onContinuous({right, right}, [&] { changeCategory(true); });
      buttonNavigator.onContinuous({left, left}, [&] { changeCategory(false); });
    }
  }

  if (hasChangedCategory) {
    topIndex = 0;
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    setCurrentSettingsForCategory();
    // Advance past any leading section headers
    while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
           (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
      const int nextIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
      if (nextIndex <= selectedSettingIndex) {
        selectedSettingIndex = settingsCount;
        break;
      }
      selectedSettingIndex = nextIndex;
    }
  }
}

bool SettingsActivity::handleHomeGesture() {
  // The edge swipe exits Settings directly; the Home key still steps back
  // through its selection and submenus. Save before leaving the whole stack.
  if (mappedInput.wasBottomEdgeUpSwipe() && !optionPopup.isActive()) {
    SETTINGS.saveToFile();
    onGoHome();
  } else if (optionPopup.isActive()) {
    optionPopup.dismiss(mappedInput, [this] { requestUpdate(); });
  } else if (!isFileBrowserView() && activeSubmenu != SettingAction::None) {
    closeSubmenu();
    requestUpdate();
  } else if (!isFileBrowserView() && selectedSettingIndex > 0) {
    selectedSettingIndex = 0;
    showSettingSelection = true;
    requestUpdate();
  } else {
    closeRootSettings();
  }
  return true;
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  if (isPageGestureSettingDisabled(setting)) return;
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.value16Ptr == &CrossPointSettings::frontlightScheduleStart ||
      setting.value16Ptr == &CrossPointSettings::frontlightScheduleEnd) {
    openFrontlightScheduleTimePicker(setting.value16Ptr, setting.nameId);
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    openLineHeightPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::characterSpacing) {
    openCharacterSpacingPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::wordSpacing) {
    openWordSpacingPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::screenMarginVertical ||
      setting.valuePtr == &CrossPointSettings::screenMarginHorizontal) {
    openScreenMarginPicker(setting);
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::readingIdleTimeThresholdUnits) {
    openIdleTimeThresholdPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
    startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), [this](const ActivityResult&) {
      SETTINGS.saveToFile();
      requestUpdate();
    });
    return;
  }
  if (setting.type == SettingType::STRING) {
    openStringEditor(setting);
    return;
  }
  if (setting.nameId == StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM) {
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult&) {
                             SETTINGS.saveToFile();
                             rebuildSettingsLists();
                           });
    return;
  }

  if (currentSettingUsesOptionMenu(setting)) {
    openEnumOptionPicker(setting);
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, currentValue);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
    QuickActions::settingChanged(SETTINGS, setting.valuePtr);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::ScreenCalibration: {
        auto activity = makeUniqueNoThrow<ScreenCalibrationActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "Failed to allocate screen calibration");
          break;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) {
          RenderLock lock(*this);
          app.setDevice(uiTarget.deviceContext());
          requestUpdate();
        });
        break;
      }
      case SettingAction::ReleaseNotes: {
        auto notes =
            makeUniqueNoThrow<ReleaseNotesActivity>(renderer, mappedInput, ReleaseNotesActivity::Mode::FromSettings);
        if (!notes) {
          LOG_ERR("SET", "OOM: Release Notes activity");
          return;
        }
        startActivityForResult(std::move(notes), [this](const ActivityResult& result) {
          const auto* menu = std::get_if<MenuResult>(&result.data);
          if (!result.isCancelled && menu && menu->action == ReleaseNotesActivity::RESULT_OPEN_LANGUAGE_DOWNLOADS) {
            if (activeSubmenu == SettingAction::None) openSubmenu(SettingAction::SystemDevice);
            pendingLanguageDownloads = true;  // Handled in loop(), once the Settings screen is current again.
          }
          requestUpdate();
        });
        break;
      }
      case SettingAction::About: {
        // The bounded activity owns its snapshot and UI host only while open.
        auto about = makeUniqueNoThrow<AboutActivity>(renderer, mappedInput);
        if (!about) {
          LOG_ERR("SET", "OOM: About activity");
          return;
        }
        startActivityForResult(std::move(about), [this](const ActivityResult&) { requestUpdate(); });
        break;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsReader:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, true), resultHandler);
        break;
      case SettingAction::DisplayStatusBar: {
        auto activity = makeUniqueNoThrow<StatusBarSettingsActivity>(renderer, mappedInput, false, false, true);
        if (!activity) {
          LOG_ERR("SET", "Failed to allocate status bar settings");
          break;
        }
        startActivityForResult(std::move(activity), resultHandler);
        break;
      }
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Plugins: {
        static constexpr StrId OPTIONS[] = {StrId::STR_ENABLED, StrId::STR_DISABLED, StrId::STR_CANCEL};
        optionPopup.show(StrId::STR_PLUGINS, OPTIONS, 3, SETTINGS.pluginsEnabled ? 0 : 1, [this](const int idx) {
          if (idx == 2) return;
          SETTINGS.pluginsEnabled = idx == 0 ? 1 : 0;
          pluginevents::refreshSubscriptions();
          if (SETTINGS.pluginsEnabled) SETTINGS.pluginHubPromptHidden = 0;
          SETTINGS.saveToFile();
          rebuildSettingsLists();
        });
        requestUpdate();
        return;
      }
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                               [](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 // Settings only manages credentials; no parent needs the connection.
                                 // Cancelled selections already stop WiFi in the picker.
                                 if (WiFi.getMode() == WIFI_MODE_NULL) return;
                                 WiFi.disconnect(false);
                                 delay(30);
                                 if (!WiFi.mode(WIFI_OFF)) {
                                   LOG_ERR("SET", "Failed to stop WiFi after network settings");
                                 }
                               });
        break;
      case SettingAction::BackupStats:
        startActivityForResult(std::make_unique<BackupStatsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ResetGlobalStats:
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_RESET_ALL_TIME_STATS),
                                                   tr(STR_RESET_ALL_TIME_STATS_CONFIRM)),
            [this](const ActivityResult& result) {
              if (!result.isCancelled && !GlobalReadingStats::resetLocal()) {
                LOG_ERR("SET", "Failed to reset all-time reading stats");
              }
              requestUpdate();
            });
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        silentRestartToNetwork(NetworkBootTarget::OTA);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        silentRestartToManageFonts();
        break;
      case SettingAction::TtfRendering: {
#if CROSSINK_SCALABLE_FONTS
        auto activity =
            makeUniqueNoThrow<TtfRenderOptionsActivity>(renderer, mappedInput, SETTINGS.sdFontFamilyName, false);
        if (!activity) {
          LOG_ERR("SET", "Failed to allocate TTF rendering settings");
          break;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
          if (const auto* options = std::get_if<TtfRenderOptionsResult>(&result.data)) {
            ttfRenderingChanged = ttfRenderingChanged || options->activeFamilyChanged;
          }
          rebuildSettingsLists();
        });
#endif
        break;
      }
      case SettingAction::FilenameFallbackFont:
        openFilenameFontPicker();
        break;
      case SettingAction::ManageHyphenation:
        if (auto manager = makeUniqueNoThrow<HyphenationManagerActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(manager), [](const ActivityResult&) {});
        } else {
          LOG_ERR("HYPH", "OOM: hyphenation manager");
        }
        break;
      case SettingAction::DownloadLanguages:
        openLanguageDownloads();
        break;
      case SettingAction::Language:
        openLanguagePicker();
        break;
      case SettingAction::KeyboardLayouts:
        startActivityForResult(std::make_unique<KeyboardLayoutsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ClockSync:
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::QuickActions:
        startActivityForResult(std::make_unique<QuickActionsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderFontOptions:
      case SettingAction::ReaderPageLayout:
      case SettingAction::ScreenMargin:
      case SettingAction::ControlsPowerButton:
      case SettingAction::ControlsHomeButton:
      case SettingAction::ControlsFrontButtons:
      case SettingAction::ControlsSideButtons:
      case SettingAction::ControlsTapsGestures:
      case SettingAction::ControlsTwoFingerSwipe:
      case SettingAction::ControlsEdgeGestures:
      case SettingAction::SystemDevice:
      case SettingAction::SystemAdvanced:
      case SettingAction::SystemFilesCache:
      case SettingAction::SystemReadingStats:
      case SettingAction::SystemGlobalStats:
      case SettingAction::DisplaySleepScreen:
      case SettingAction::DisplayFrontlight:
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else if (setting.type == SettingType::SUBMENU) {
    openSubmenu(setting.action);
    return;
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  if (isTwoFingerSwipeSetting(setting.valuePtr)) {
    CrossPointSettings::normalizeTwoFingerSwipeActions(SETTINGS, setting.valuePtr);
  }
  QuickActions::settingChanged(SETTINGS, setting.valuePtr);
  SETTINGS.saveToFile();
  // Apply this while `setting` still refers to the current list; rebuilding
  // below clears its backing vector and invalidates the reference.
  applyUiSettingChange(setting.valuePtr);
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/true,
          /*showPercentValue=*/false, StrId::STR_NEVER, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true, /*valueFormatter=*/nullptr, /*tapStep=*/0,
          /*useReaderSlider=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openLineHeightPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "LineHeightInterval", StrId::STR_LINE_SPACING, SETTINGS.lineHeightPercent,
          CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT, 1, 5,
          StrId::STR_NONE_OPT, /*readerActivity=*/false,
          /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/5, /*useReaderSlider=*/true,
          IntervalSelectionActivity::ReaderPreviewSetting::LineSpacing),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openFrontlightScheduleTimePicker(uint16_t CrossPointSettings::* const valuePtr,
                                                        const StrId titleId) {
  const uint16_t storedValue = SETTINGS.*valuePtr;
  startActivityForResult(std::make_unique<FrontlightTimePickerActivity>(renderer, mappedInput, titleId, storedValue),
                         [this, valuePtr](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             SETTINGS.*valuePtr = static_cast<uint16_t>(std::get<IntervalResult>(result.data).value);
                             SETTINGS.saveToFile();
                           }
                           requestUpdate();
                         });
}

void SettingsActivity::openIdleTimeThresholdPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "IdleTimeThresholdInterval", StrId::STR_IDLE_TIME_THRESHOLD,
          SETTINGS.getReadingIdleTimeThresholdSeconds(), CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS, 60, StrId::STR_SECONDS_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false,
          /*showTouchHeaderBackButton=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.readingIdleTimeThresholdUnits = CrossPointSettings::readingIdleTimeThresholdUnitsForSeconds(
              static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (isPageGestureSettingDisabled(setting)) return tr(STR_DISABLED);
  if (setting.action == SettingAction::Plugins) return SETTINGS.pluginsEnabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  if (settingShowsNavigationCaret(setting)) return ">";
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_ON) : tr(STR_OFF);
  }
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t displayIndex = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
    return sideButtonOptionLabel(setting, displayIndex);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    return settingEnumOptionLabel(setting, setting.valueGetter());
  }
  if (setting.type == SettingType::VALUE && (setting.valuePtr != nullptr || setting.value16Ptr != nullptr)) {
    return formatSettingValue(setting);
  }
  if (setting.type == SettingType::ACTION && setting.action == SettingAction::FilenameFallbackFont) {
    return SETTINGS.filenameFallbackFont[0] ? SETTINGS.filenameFallbackFont : tr(STR_NONE_OPT);
  }
  if (setting.type == SettingType::ACTION && setting.action == SettingAction::Language) {
    return I18N.getName();
  }
  if (setting.type == SettingType::STRING) {
    if (setting.nameId == StrId::STR_DEVICE_NAME) return SETTINGS.getEffectiveDeviceName();
    if (setting.stringGetter) return setting.stringGetter();
    if (setting.stringMaxLen > 0) return reinterpret_cast<const char*>(&SETTINGS) + setting.stringOffset;
  }
  return "";
}

void SettingsActivity::settingsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<SettingsActivity*>(user)->buildSettingsScreen(screen);
}

void SettingsActivity::buildSettingsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
#if CROSSINK_APP_CAP_TOUCH
  const bool landscapeTouch = useLandscapeTouchLayout(renderer);
#endif
  const fui::Rect safe = screen.frame().safeRect();
  // setContentMargin() is relative to the bezel-safe rectangle, while the
  // compact header geometry is in absolute screen coordinates. Overlap the
  // tab's top rule with the header's final underline pixel.
  const int tabTop = std::max<int>(safe.y, CompactHeader::headerBottomY(renderer) - 1);
  setUiContentMargin(
      screen, renderer,
      fui::Insets{static_cast<int16_t>(tabTop), 0, static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});

  if (isFileBrowserView()) {
    const int16_t listInset = static_cast<int16_t>(metrics.listInset);
    screen.insetContent(fui::Insets{0, listInset, 0, listInset});
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
    visibleRows = settingsCount;
    topIndex = 0;
    for (int i = 0; i < settingsCount; ++i) {
      const auto& setting = (*currentSettings)[i];
      fui::SettingRowProps row;
      row.label = I18N.get(setting.nameId);
      row.action = ACTION_ROW;
      row.valueId = static_cast<int16_t>(i);
      row.labelText = screen.theme().bodyText;
      row.valueText = screen.theme().bodyText;
      row.sidePadding = static_cast<int16_t>(metrics.listSidePadding);
      row.state = showSettingSelection && selectedSettingIndex == i + 1 ? fui::StateSelected : fui::StateNormal;
      if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
        fui::ToggleRowProps toggle;
        toggle.row = row;
        toggle.checked = SETTINGS.*(setting.valuePtr);
        toggle.toggleAction = ACTION_ROW;
        toggle.toggleValue = static_cast<int16_t>(i);
        screen.toggleRow(toggle);
      } else {
        const std::string value = settingValueText(setting);
        row.value = value.c_str();
        screen.settingRow(row);
      }
    }
    return;
  }

  // Category tabs. The selected pill dims to a dither when the selection is
  // down in the list (the legacy focused/unfocused tab distinction).
  fui::TabItem tabs[categoryCount];
  for (int i = 0; i < categoryCount; i++) {
    tabs[i].label = I18N.get(categoryNames[i]);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = selectedCategoryIndex == i;
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = categoryCount;
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  // Small text: four category labels share the band, the body font truncates.
  tabProps.text = screen.theme().smallText;
  // Pill wraps the label with real padding; the band grows with the scaled
  // font when the theme's tabBarHeight is too short for it. Keep the horizontal
  // padding tight: four equal slots share the band, so wide labels (e.g.
  // "Controls") truncate to an ellipsis at large UI scales when the pill eats
  // too much width. Vertical padding stays for the pill height.
  tabProps.tabInset = fui::Insets{2, 2, 4, 2};
  tabProps.contentInset = fui::Insets{2, 4, 2, 4};
  const int16_t tabLineHeight = screen.target().lineHeight(screen.theme().smallText.font);
  const int16_t preferredTabHeight =
      mappedInput.hasTouch() ? TOUCH_TAB_BAR_HEIGHT : static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;
  // Legacy Lyra two-state treatment: with the selection on the tab band, the
  // band fills gray and the active tab is a solid pill; with the selection
  // down in the list, the band is plain and the active tab keeps a gray box
  // with an underline. The 1px rule under the band is always there.
  const bool tabsFocused = selectedSettingIndex == 0;
  const bool borderedTabs = metrics.tabBarAppearance == ThemeTabBarAppearance::BorderedText;
  const bool roundedRaffTabs = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::ROUNDEDRAFF;
  tabProps.divider = true;
  fui::StyleSet tabStyles;
  if (roundedRaffTabs) {
    // RoundedRaff's tabs have always sat on white, with the selected pill
    // turning dark gray after focus moves into the settings list.
    tabStyles.explicitlySet = true;
    tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.background = fui::Paint::solid(tabsFocused ? fui::Color::Black : fui::Color::DarkGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = 18;
    tabStyles.focused = tabStyles.selected;
    tabStyles.active = tabStyles.selected;
    tabProps.tabStyles = tabStyles;
  } else if (!borderedTabs) {
    tabStyles.explicitlySet = true;
    tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (tabsFocused) {
      tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
      tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
      tabStyles.selected.radius = screen.theme().listRowRadius;
    } else {
      tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
      tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
      // Let the selected underline meet the shared bottom divider, as in the
      // original Lyra tab bar. The default bottom inset leaves a visible gap.
      tabProps.tabInset.bottom = 0;
      tabProps.selectedUnderline = 2;
    }
    // Focus/flash states keep the pill instead of falling back to an unset
    // (blank) style.
    tabStyles.focused = tabStyles.selected;
    tabStyles.active = tabStyles.selected;
    tabProps.tabStyles = tabStyles;
  }
#if CROSSINK_APP_CAP_TOUCH
  if (landscapeTouch) {
    // Landscape has width to spare but little vertical room. Keep categories
    // in a left rail so the settings list can use the full remaining height.
    const fui::Rect body = screen.body();
    const int16_t railWidth = static_cast<int16_t>(body.width / 4);
    int16_t tabY = body.y;
    for (int i = 0; i < categoryCount; ++i) {
      const int16_t tabHeight =
          static_cast<int16_t>(i == categoryCount - 1 ? body.bottom() - tabY : body.height / categoryCount);
      const fui::Rect tabRect{body.x, tabY, railWidth, tabHeight};
      fui::TabBarProps railProps = tabProps;
      railProps.tabs = &tabs[i];
      railProps.count = 1;
      if (!roundedRaffTabs && !borderedTabs && tabsFocused) {
        screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
      }
      drawUiTabBar(screen, railProps, tabRect, metrics.tabBarAppearance);
      tabY = static_cast<int16_t>(tabY + tabHeight);
    }
    screen.target().fill(fui::Rect{static_cast<int16_t>(body.x + railWidth - 1), body.y, 1, body.height},
                         fui::Paint::solid(fui::Color::Black));
    screen.insetContent(fui::Insets{0, 0, 0, static_cast<int16_t>(railWidth + metrics.verticalSpacing)});
  } else
#endif
  {
    const fui::Rect tabRect = screen.takeTop(tabBand);
    if (!roundedRaffTabs && !borderedTabs && tabsFocused) {
      screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
    }
    drawUiTabBar(screen, tabProps, tabRect, metrics.tabBarAppearance);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }

  const StrId submenuTitle = activeSubmenuTitleId();
  if (submenuTitle != StrId::STR_NONE_OPT) {
    fui::TextStyle titleStyle = screen.theme().smallText;
    titleStyle.bold = true;
    titleStyle.maxLines = 1;
    const int16_t titleHeight = screen.target().lineHeight(titleStyle.font);
    fui::Rect titleRect = screen.takeTop(titleHeight, static_cast<int16_t>(metrics.verticalSpacing));
    const int16_t sidePadding = static_cast<int16_t>(metrics.contentSidePadding);
    titleRect.x = static_cast<int16_t>(titleRect.x + sidePadding);
    titleRect.width = static_cast<int16_t>(titleRect.width > sidePadding * 2 ? titleRect.width - sidePadding * 2 : 0);
    screen.target().text(titleRect, I18N.get(submenuTitle), titleStyle);
  }

  // Settings rows. Values are built per render and owned for the draw only.
  const auto& settings = *currentSettings;
  const std::string leftUpLabel = sideButtonGroupLabel(true);
  const std::string rightDownLabel = sideButtonGroupLabel(false);
  std::vector<std::string> values(settings.size());
  std::vector<fui::ListItem> items;
  items.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    values[i] = settingValueText(settings[i]);
    const bool isSectionHeader = settings[i].type == SettingType::SECTION_HEADER;
    fui::ListItem item;
    item.enabled = !isPageGestureSettingDisabled(settings[i]);
    const fui::BitmapRef directionIcon = swipeActionIcon(settings[i].nameId);
    const fui::BitmapRef endpointIcon = frontlightScheduleEndpointIcon(settings[i]);
    const fui::BitmapRef itemIcon = directionIcon ? directionIcon : endpointIcon;
    const char* sectionLabel =
        activeSubmenu == SettingAction::ControlsSideButtons
            ? (settings[i].nameId == StrId::STR_DIR_LEFT ? leftUpLabel.c_str() : rightDownLabel.c_str())
            : I18N.get(settings[i].nameId);
    item.label = isSectionHeader ? uiListSectionHeaderLabel(values[i], sectionLabel)
                                 : (directionIcon ? "" : I18N.get(settings[i].nameId));
    item.icon = itemIcon;
    if (!isSectionHeader && !values[i].empty()) item.value = values[i].c_str();
    item.isHeader = isSectionHeader;
    item.toggle = !isSectionHeader && settings[i].type == SettingType::TOGGLE;
    if (item.toggle) {
      item.toggleChecked = settings[i].valuePtr != nullptr && SETTINGS.*(settings[i].valuePtr) != 0;
      item.value = nullptr;
    }
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedSettingIndex - 1);  // -1 = tab band focused
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Use FreeInkUI's bodyText default so Settings scales consistently with
  // File Browser, reader menus, and the other list-style screens.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  if (activeSubmenu == SettingAction::ControlsSideButtons) {
    props.headerText = screen.theme().smallText;
    props.headerText.bold = true;
  }
  configureUiListSectionHeaders(props, screen.theme());
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, settingsCount);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* title = isFileBrowserView() ? tr(STR_FILE_BROWSER_SETTINGS) : tr(STR_SETTINGS_TITLE);

  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::drawCompact(renderer, title, false, false);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }

  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  // Keep build information discoverable without crowding the common header.
  if (!isFileBrowserView() && selectedCategoryIndex == 3) {
    drawSystemVersionFooter(renderer, metrics);
  }

  const auto confirmLabel =
      (!isFileBrowserView() && selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 &&
                     (currentSettingUsesOptionMenu((*currentSettings)[selectedSettingIndex - 1]) ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SUBMENU ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::ACTION ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_FONT_FAMILY ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::STRING ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr == &CrossPointSettings::lineHeightPercent ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr ==
                          &CrossPointSettings::readingIdleTimeThresholdUnits ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr ==
                          &CrossPointSettings::screenMarginVertical ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr ==
                          &CrossPointSettings::screenMarginHorizontal ||
                      (*currentSettings)[selectedSettingIndex - 1].value16Ptr ==
                          &CrossPointSettings::frontlightScheduleStart ||
                      (*currentSettings)[selectedSettingIndex - 1].value16Ptr ==
                          &CrossPointSettings::frontlightScheduleEnd)
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));

  const bool horizontalFront = SETTINGS.menuNavigation == CrossPointSettings::MENU_NAV_DIRECTIONAL &&
                               !isFileBrowserView() &&
                               mappedInput.menuButton(MappedInputManager::Button::Up) == MappedInputManager::Button::Up;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel,
                                            (horizontalFront ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP)),
                                            (horizontalFront ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}

#ifdef SIMULATOR
void SettingsActivity::simulatorOpenFrontButtons() {
  controlsFrontButtonSettings = buildControlsFrontButtonSettingsList(getSettingsList());
  selectedCategoryIndex = 2;
  openSubmenu(SettingAction::ControlsFrontButtons);
}
#endif
