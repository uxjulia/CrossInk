#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "AppVersion.h"
#include "BackupStatsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/HeaderDate.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

namespace {
constexpr int systemVersionFooterSideMargin = 20;
constexpr int systemVersionFooterBottomInset = 15;

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

void drawCenteredTextLine(const GfxRenderer& renderer, const int pageWidth, const int y, const std::string& text) {
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
  const int labelX = (pageWidth - labelWidth) / 2;
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

void drawSystemVersionFooter(const GfxRenderer& renderer, const int pageWidth, const int pageHeight,
                             const ThemeMetrics& metrics) {
  const std::string label = "CrossInk " CROSSINK_VERSION;
  const int maxWidth = pageWidth - systemVersionFooterSideMargin * 2;
  const int bottomLineY =
      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - systemVersionFooterBottomInset;

  if (renderer.getTextWidth(SMALL_FONT_ID, label.c_str()) <= maxWidth) {
    drawCenteredTextLine(renderer, pageWidth, bottomLineY, label);
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
  drawCenteredTextLine(renderer, pageWidth, bottomLineY - lineHeight, firstLine);
  drawCenteredTextLine(renderer, pageWidth, bottomLineY, secondLine);
}

std::string formatSettingValue(const SettingInfo& setting) {
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return tr(STR_SLEEP_NEVER);
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
  return std::to_string(SETTINGS.*(setting.valuePtr));
}

uint8_t valueDisplayIndexForRawValue(const SettingInfo& setting, const uint8_t rawValue) {
  const uint8_t min = setting.valueRange.min;
  const uint8_t max = setting.valueRange.max;
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  const uint8_t clampedValue = std::clamp(rawValue, min, max);
  const uint8_t offset = clampedValue > min ? clampedValue - min : 0;
  return static_cast<uint8_t>((offset + step / 2) / step);
}

uint8_t rawValueForValueDisplayIndex(const SettingInfo& setting, const uint8_t displayIndex) {
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  const uint16_t rawValue = static_cast<uint16_t>(setting.valueRange.min) + static_cast<uint16_t>(displayIndex) * step;
  return static_cast<uint8_t>(std::min<uint16_t>(rawValue, setting.valueRange.max));
}

uint8_t valueOptionCount(const SettingInfo& setting) {
  const uint8_t step = setting.valueRange.step == 0 ? 1 : setting.valueRange.step;
  return static_cast<uint8_t>(((setting.valueRange.max - setting.valueRange.min) / step) + 1);
}
}  // namespace

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  displaySleepSettings.clear();
  readerSettings.clear();
  readerFontSettings.clear();
  readerPageLayoutSettings.clear();
  controlsSettings.clear();
  controlsPowerSettings.clear();
  controlsFrontButtonSettings.clear();
  controlsSideButtonSettings.clear();
  systemSettings.clear();
  systemDeviceSettings.clear();
  systemFilesCacheSettings.clear();
  systemReadingStatsSettings.clear();
  systemGlobalStatsSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  const auto allSettings = getSettingsList(&sdFontSystem.registry());
  displaySettings = buildGroupedDisplaySettingsList(allSettings);
  displaySleepSettings = buildDisplaySleepSettingsList(allSettings);
  readerSettings = buildReaderSettingsParentList(allSettings);
  readerFontSettings = buildReaderFontSettingsList(allSettings);
  readerPageLayoutSettings = buildReaderPageLayoutSettingsList(allSettings);
  systemSettings = buildSystemSettingsParentList(allSettings);
  systemDeviceSettings = buildSystemDeviceSettingsList(allSettings);
  systemFilesCacheSettings = buildSystemFilesCacheSettingsList(allSettings);
  systemReadingStatsSettings = buildSystemReadingStatsSettingsList(allSettings);
  systemGlobalStatsSettings = buildSystemGlobalStatsSettingsList(allSettings);
  controlsSettings = buildControlsSettingsParentList(allSettings);
  controlsPowerSettings = buildControlsPowerSettingsList(allSettings);
  controlsFrontButtonSettings = buildControlsFrontButtonSettingsList(allSettings);
  controlsSideButtonSettings = buildControlsSideButtonSettingsList(allSettings);

  if (controlsPowerSettings.size() != 2 || controlsFrontButtonSettings.size() != 5 ||
      controlsSideButtonSettings.size() != 3) {
    LOG_ERR("SET", "Unexpected controls submenu counts: power=%u front=%u side=%u",
            static_cast<uint32_t>(controlsPowerSettings.size()),
            static_cast<uint32_t>(controlsFrontButtonSettings.size()),
            static_cast<uint32_t>(controlsSideButtonSettings.size()));
  }

  setCurrentSettingsForCategory();
}

void SettingsActivity::setCurrentSettingsForCategory() {
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = activeSubmenu == SettingAction::DisplaySleepScreen ? &displaySleepSettings : &displaySettings;
      break;
    case 1:
      switch (activeSubmenu) {
        case SettingAction::ReaderFontOptions:
          currentSettings = &readerFontSettings;
          break;
        case SettingAction::ReaderPageLayout:
          currentSettings = &readerPageLayoutSettings;
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
        case SettingAction::ControlsFrontButtons:
          currentSettings = &controlsFrontButtonSettings;
          break;
        case SettingAction::ControlsSideButtons:
          currentSettings = &controlsSideButtonSettings;
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
}

StrId SettingsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::DisplaySleepScreen:
      return StrId::STR_DISPLAY_SLEEP_SCREEN;
    case SettingAction::ReaderFontOptions:
      return StrId::STR_READER_FONT_OPTIONS;
    case SettingAction::ReaderPageLayout:
      return StrId::STR_READER_PAGE_LAYOUT;
    case SettingAction::ControlsPowerButton:
      return StrId::STR_POWER_BUTTON;
    case SettingAction::ControlsFrontButtons:
      return StrId::STR_FRONT_BUTTONS;
    case SettingAction::ControlsSideButtons:
      return StrId::STR_SIDE_BUTTONS;
    case SettingAction::SystemDevice:
      return StrId::STR_SYSTEM_DEVICE;
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
  setCurrentSettingsForCategory();
  selectedSettingIndex = 1;
  while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
         (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
  }
}

void SettingsActivity::closeSubmenu() {
  activeSubmenu = parentSubmenu;
  parentSubmenu = SettingAction::None;
  setCurrentSettingsForCategory();
  selectedSettingIndex = 1;
}

bool SettingsActivity::currentSettingUsesOptionMenu(const SettingInfo& setting) const {
  return (selectedCategoryIndex == 0 || selectedCategoryIndex == 1 || selectedCategoryIndex == 2) &&
         setting.nameId != StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM &&
         settingEnumOptionCount(setting) > 2 &&
         (setting.valuePtr != nullptr || (setting.valueGetter && setting.valueSetter));
}

void SettingsActivity::openEnumOptionPicker(const SettingInfo& setting) {
  const size_t optionCount = settingEnumOptionCount(setting);
  if (optionCount == 0) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (uint8_t i = 0; i < optionCount; i++) {
    options.push_back(settingEnumOptionLabel(setting, i));
  }

  uint8_t currentIndex = 0;
  if (setting.valuePtr != nullptr) {
    currentIndex = enumDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
  } else if (setting.valueGetter) {
    currentIndex = setting.valueGetter();
  }
  if (currentIndex >= optionCount) currentIndex = 0;

  const SettingInfo selectedSetting = setting;
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "SettingsOptionSelect", setting.nameId,
                                                std::move(options), currentIndex),
      [this, selectedSetting](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection == nullptr) {
          requestUpdate();
          return;
        }

        if (selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) = enumRawValueForDisplayIndex(selectedSetting, selection->index);
        } else if (selectedSetting.valueSetter) {
          selectedSetting.valueSetter(selection->index);
        }

        const bool sleepScreenChanged = selectedSetting.valuePtr == &CrossPointSettings::sleepScreen;
        const bool quickResumeTimeoutChanged = selectedSetting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        requestUpdate();
      });
}

void SettingsActivity::openScreenMarginPicker(const SettingInfo& setting) {
  const uint8_t optionCount = valueOptionCount(setting);
  if (optionCount == 0 || setting.valuePtr == nullptr) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (uint8_t i = 0; i < optionCount; i++) {
    options.push_back(std::to_string(rawValueForValueDisplayIndex(setting, i)));
  }

  uint8_t currentIndex = valueDisplayIndexForRawValue(setting, SETTINGS.*(setting.valuePtr));
  if (currentIndex >= optionCount) currentIndex = 0;

  const SettingInfo selectedSetting = setting;
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "SettingsValueSelect", selectedSetting.nameId,
                                                std::move(options), currentIndex),
      [this, selectedSetting](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection != nullptr && selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) = rawValueForValueDisplayIndex(selectedSetting, selection->index);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  activeSubmenu = SettingAction::None;
  parentSubmenu = SettingAction::None;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
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
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
           (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
      selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    while (selectedSettingIndex > 0 && selectedSettingIndex <= settingsCount &&
           (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SECTION_HEADER) {
      selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    }
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    enterCategory(ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    enterCategory(ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount));
    requestUpdate();
  });

  if (hasChangedCategory) {
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

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    openLineHeightPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::screenMargin) {
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
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsReader:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, true), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
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
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ClockSync:
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderFontOptions:
      case SettingAction::ReaderPageLayout:
      case SettingAction::ControlsPowerButton:
      case SettingAction::ControlsFrontButtons:
      case SettingAction::ControlsSideButtons:
      case SettingAction::SystemDevice:
      case SettingAction::SystemFilesCache:
      case SettingAction::SystemReadingStats:
      case SettingAction::SystemGlobalStats:
      case SettingAction::DisplaySleepScreen:
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
  SETTINGS.saveToFile();
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
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, StrId::STR_SLEEP_TIMER_STEP_HINT,
          SETTINGS.sleepTimeoutMinutes, CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
          CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/true,
          /*showPercentValue=*/false, StrId::STR_SLEEP_NEVER),
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
          renderer, mappedInput, "LineHeightInterval", StrId::STR_LINE_SPACING, StrId::STR_PERCENT_STEP_HINT,
          SETTINGS.lineHeightPercent, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
          CrossPointSettings::MAX_LINE_HEIGHT_PERCENT, 1, 10, StrId::STR_NONE_OPT, /*readerActivity=*/false,
          /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openIdleTimeThresholdPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "IdleTimeThresholdInterval", StrId::STR_IDLE_TIME_THRESHOLD,
          StrId::STR_IDLE_TIME_THRESHOLD_STEP_HINT, SETTINGS.getReadingIdleTimeThresholdSeconds(),
          CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_SECONDS,
          CrossPointSettings::READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS, 60, StrId::STR_SECONDS_VALUE_FORMAT,
          /*readerActivity=*/false, /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false,
          /*showPercentValue=*/false),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.readingIdleTimeThresholdUnits = CrossPointSettings::readingIdleTimeThresholdUnitsForSeconds(
              static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE));
  drawHeaderDate(renderer, pageWidth, metrics);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  Rect listRect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing,
                pageWidth,
                pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                              metrics.buttonHintsHeight + metrics.verticalSpacing * 2)};
  const StrId submenuTitleId = activeSubmenuTitleId();
  if (submenuTitleId != StrId::STR_NONE_OPT) {
    constexpr int submenuHeaderFontId = UI_10_FONT_ID;
    const int headerLineHeight = renderer.getLineHeight(submenuHeaderFontId);
    const int headerOffset = headerLineHeight + metrics.verticalSpacing;
    const int headerMaxWidth = listRect.width - metrics.contentSidePadding * 2;
    const auto headerLabel =
        renderer.truncatedText(submenuHeaderFontId, I18N.get(submenuTitleId), headerMaxWidth, EpdFontFamily::BOLD);
    renderer.drawText(submenuHeaderFontId, listRect.x + metrics.contentSidePadding, listRect.y, headerLabel.c_str(),
                      true, EpdFontFamily::BOLD);
    listRect.y += headerOffset;
    listRect.height = std::max(0, listRect.height - headerOffset);
  }
  GUI.drawList(
      renderer, listRect, settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [this, &settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (settingShowsNavigationCaret(setting)) {
          valueText = ">";
        } else if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          const uint8_t displayValue = enumDisplayIndexForRawValue(setting, value);
          const size_t optionCount = settingEnumOptionCount(setting);
          const uint8_t safeValue = displayValue < optionCount ? displayValue : 0;
          valueText = settingEnumOptionLabel(setting, safeValue);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          valueText = settingEnumOptionLabel(setting, value);
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = formatSettingValue(setting);
        }
        return valueText;
      },
      true, nullptr, [&settings](int i) { return settings[i].type == SettingType::SECTION_HEADER; });

  // Draw CrossInk version label at the bottom of the System tab
  if (selectedCategoryIndex == 3) {
    drawSystemVersionFooter(renderer, pageWidth, pageHeight, metrics);
  }

  // Draw help text
  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 &&
                     (currentSettingUsesOptionMenu((*currentSettings)[selectedSettingIndex - 1]) ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::SUBMENU ||
                      (*currentSettings)[selectedSettingIndex - 1].type == SettingType::ACTION ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_FONT_FAMILY ||
                      (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr == &CrossPointSettings::lineHeightPercent ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr ==
                          &CrossPointSettings::readingIdleTimeThresholdUnits ||
                      (*currentSettings)[selectedSettingIndex - 1].valuePtr == &CrossPointSettings::screenMargin)
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
