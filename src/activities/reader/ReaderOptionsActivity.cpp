#include "ReaderOptionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
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
}  // namespace

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();

  rebuildSettingsList();
  requestUpdate();
}

void ReaderOptionsActivity::rebuildSettingsList() {
  settings.clear();
  sdFontSystem.refreshIfDirty();
  const auto allSettings = getSettingsList(&sdFontSystem.registry());
  settings.reserve(allSettings.size() + 2);
  std::copy_if(allSettings.begin(), allSettings.end(), std::back_inserter(settings),
               [](const auto& s) { return s.category == StrId::STR_CAT_READER; });

  const auto fontSizeSetting = std::find_if(settings.begin(), settings.end(),
                                            [](const auto& setting) { return setting.nameId == StrId::STR_FONT_SIZE; });
  const auto manageFontsSetting = SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts);
  settings.insert(fontSizeSetting == settings.end() ? settings.end() : fontSizeSetting + 1, manageFontsSetting);
  settings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  settingsCount = static_cast<int>(settings.size());
  selectedIndex = 0;
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= settingsCount) return;
  const auto& setting = settings[selectedIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, cur);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               sdFontSystem.refreshIfDirty();
                               rebuildSettingsList();
                               requestUpdate();
                             });
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = cur + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (setting.action == SettingAction::DownloadFonts) {
      startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               sdFontSystem.refreshIfDirty();
                               rebuildSettingsList();
                               requestUpdate();
                             });
      return;
    }
    if (setting.action == SettingAction::CustomiseStatusBar) {
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [](const ActivityResult&) { SETTINGS.saveToFile(); });
      return;
    }
  }
}

void ReaderOptionsActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, settingsCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, settingsCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
}

void ReaderOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.buttonHintsHeight : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;

  GUI.drawHeader(renderer, Rect{contentX, metrics.topPadding, contentWidth, metrics.headerHeight},
                 tr(STR_READER_OPTIONS), nullptr);

  GUI.drawList(
      renderer,
      Rect{contentX, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, contentWidth,
           pageHeight -
               (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight + metrics.verticalSpacing * 2)},
      settingsCount, selectedIndex, [this](int i) { return std::string(I18N.get(settings[i].nameId)); }, nullptr,
      nullptr,
      [this](int i) {
        const auto& setting = settings[i];
        std::string valueText;
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          valueText = SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
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
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        }
        return valueText;
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
