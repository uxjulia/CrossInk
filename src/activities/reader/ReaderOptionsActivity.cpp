#include "ReaderOptionsActivity.h"

#include <Epub/EpubRenderMode.h>
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
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/OptionSelectionActivity.h"
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

std::string formatSettingValue(const SettingInfo& setting) {
  if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
    return std::to_string(SETTINGS.*(setting.valuePtr)) + "%";
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

SettingInfo buildReaderRenderModeSetting() {
  return SettingInfo::Enum(
             StrId::STR_EPUB_RENDER_MODE, &CrossPointSettings::epubRenderMode,
             {StrId::STR_RENDER_MODE_CROSSINK_DEFAULT, StrId::STR_RENDER_MODE_BALANCED, StrId::STR_RENDER_MODE_LIGHT})
      .withEnumRawValues({static_cast<uint8_t>(EpubRenderMode::CrossInkDefault),
                          static_cast<uint8_t>(EpubRenderMode::Balanced), static_cast<uint8_t>(EpubRenderMode::Light)});
}
}  // namespace

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();

  activeSubmenu = SettingAction::None;
  settingsDirty = false;
  rebuildSettingsList();
  requestUpdate();
}

void ReaderOptionsActivity::rebuildSettingsList() {
  settings.clear();
  fontSettings.clear();
  pageLayoutSettings.clear();
  sdFontSystem.refreshIfDirty();
  const auto allSettings = getSettingsList(&sdFontSystem.registry());
  settings = buildReaderSettingsParentList(allSettings);
  settings.push_back(buildReaderRenderModeSetting());
  fontSettings = buildReaderFontSettingsList(allSettings);
  pageLayoutSettings = buildReaderPageLayoutSettingsList(allSettings);
  fontSettings.erase(std::remove_if(fontSettings.begin(), fontSettings.end(),
                                    [](const SettingInfo& setting) {
                                      return setting.nameId == StrId::STR_SD_FONT_SIZE_RANGE ||
                                             setting.nameId == StrId::STR_MANAGE_FONTS;
                                    }),
                     fontSettings.end());

  setCurrentSettings();
  selectedIndex = 0;
}

void ReaderOptionsActivity::persistReaderSettings() {
  if (saveSettingsCallback) {
    saveSettingsCallback(saveSettingsContext);
  } else {
    SETTINGS.saveToFile();
  }
}

void ReaderOptionsActivity::persistGlobalSettings() {
  if (saveGlobalSettingsCallback) {
    saveGlobalSettingsCallback(saveGlobalSettingsContext);
  } else {
    SETTINGS.saveToFile();
  }
}

void ReaderOptionsActivity::beginGlobalSettingsEdit() {
  if (beginGlobalSettingsEditCallback) {
    beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
  }
}

void ReaderOptionsActivity::endGlobalSettingsEdit() {
  if (endGlobalSettingsEditCallback) {
    endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
  }
}

void ReaderOptionsActivity::setCurrentSettings() {
  switch (activeSubmenu) {
    case SettingAction::ReaderFontOptions:
      currentSettings = &fontSettings;
      break;
    case SettingAction::ReaderPageLayout:
      currentSettings = &pageLayoutSettings;
      break;
    default:
      currentSettings = &settings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

StrId ReaderOptionsActivity::activeSubmenuTitleId() const {
  switch (activeSubmenu) {
    case SettingAction::ReaderFontOptions:
      return StrId::STR_READER_FONT_OPTIONS;
    case SettingAction::ReaderPageLayout:
      return StrId::STR_READER_PAGE_LAYOUT;
    default:
      return StrId::STR_NONE_OPT;
  }
}

void ReaderOptionsActivity::openSubmenu(SettingAction action) {
  activeSubmenu = action;
  setCurrentSettings();
  selectedIndex = 0;
}

void ReaderOptionsActivity::closeSubmenu() {
  activeSubmenu = SettingAction::None;
  setCurrentSettings();
  selectedIndex = 0;
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

void ReaderOptionsActivity::moveSelection(bool forward) {
  if (settingsCount <= 0) return;

  for (int i = 0; i < settingsCount; i++) {
    selectedIndex = forward ? ButtonNavigator::nextIndex(selectedIndex, settingsCount)
                            : ButtonNavigator::previousIndex(selectedIndex, settingsCount);
    if ((*currentSettings)[selectedIndex].type != SettingType::SECTION_HEADER) {
      break;
    }
  }
}

bool ReaderOptionsActivity::currentSettingUsesOptionMenu(const SettingInfo& setting) const {
  return setting.nameId != StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM &&
         settingEnumOptionCount(setting) > 2 &&
         (setting.valuePtr != nullptr || (setting.valueGetter && setting.valueSetter));
}

void ReaderOptionsActivity::openEnumOptionPicker(const SettingInfo& setting) {
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
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "ReaderOptionsOptionSelect", setting.nameId,
                                                std::move(options), currentIndex, true),
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

        persistReaderSettings();
        requestUpdate();
      });
}

void ReaderOptionsActivity::openScreenMarginPicker(const SettingInfo& setting) {
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
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "ReaderOptionsValueSelect",
                                                selectedSetting.nameId, std::move(options), currentIndex, true),
      [this, selectedSetting](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection != nullptr && selectedSetting.valuePtr != nullptr) {
          SETTINGS.*(selectedSetting.valuePtr) = rawValueForValueDisplayIndex(selectedSetting, selection->index);
          persistReaderSettings();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= settingsCount) return;
  const auto& setting = (*currentSettings)[selectedIndex];

  if (setting.nameId == StrId::STR_FONT_FAMILY && setting.type == SettingType::ENUM) {
    startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               persistReaderSettings();
                             }
                             sdFontSystem.refreshIfDirty();
                             rebuildSettingsList();
                             requestUpdate();
                           });
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !cur;
    settingsDirty = true;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    if (currentSettingUsesOptionMenu(setting)) {
      openEnumOptionPicker(setting);
      return;
    }
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    const uint8_t currentIndex = enumDisplayIndexForRawValue(setting, cur);
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t nextIndex = (currentIndex + 1) % static_cast<uint8_t>(optionCount);
    SETTINGS.*(setting.valuePtr) = enumRawValueForDisplayIndex(setting, nextIndex);
    settingsDirty = true;
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (currentSettingUsesOptionMenu(setting)) {
      openEnumOptionPicker(setting);
      return;
    }
    const size_t optionCount = settingEnumOptionCount(setting);
    if (optionCount == 0) return;
    const uint8_t totalValues = static_cast<uint8_t>(optionCount);
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
    settingsDirty = true;
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.valuePtr == &CrossPointSettings::lineHeightPercent) {
      openLineHeightPicker();
      return;
    }
    if (setting.valuePtr == &CrossPointSettings::screenMargin) {
      openScreenMarginPicker(setting);
      return;
    }
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = cur + setting.valueRange.step;
    }
    settingsDirty = true;
  } else if (setting.type == SettingType::ACTION) {
    if (setting.action == SettingAction::DownloadFonts) {
      if (settingsDirty) {
        persistReaderSettings();
        settingsDirty = false;
      }
      startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               persistGlobalSettings();
                               sdFontSystem.refreshIfDirty();
                               rebuildSettingsList();
                               requestUpdate();
                             });
      return;
    }
    if (setting.action == SettingAction::CustomiseStatusBar) {
      if (settingsDirty) {
        persistReaderSettings();
        settingsDirty = false;
      }
      beginGlobalSettingsEdit();
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput, true),
                             [this](const ActivityResult&) {
                               persistGlobalSettings();
                               endGlobalSettingsEdit();
                             });
      return;
    }
  } else if (setting.type == SettingType::SUBMENU) {
    openSubmenu(setting.action);
    return;
  }
}

void ReaderOptionsActivity::openLineHeightPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "ReaderOptionsLineHeightInterval", StrId::STR_LINE_SPACING,
          StrId::STR_PERCENT_STEP_HINT, SETTINGS.lineHeightPercent, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
          CrossPointSettings::MAX_LINE_HEIGHT_PERCENT, 1, 10, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
              static_cast<uint8_t>(std::get<IntervalResult>(result.data).value));
          persistReaderSettings();
        }
        requestUpdate();
      });
}

void ReaderOptionsActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    moveSelection(true);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    moveSelection(false);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (activeSubmenu != SettingAction::None) {
      closeSubmenu();
      requestUpdate();
      return;
    }
    if (settingsDirty) {
      persistReaderSettings();
      settingsDirty = false;
    }
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
                 tr(STR_READER_OPTIONS), nullptr, true);

  const auto& visibleSettings = *currentSettings;
  Rect listRect{contentX, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, contentWidth,
                pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight +
                              metrics.verticalSpacing * 2)};
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
      renderer, listRect, settingsCount, selectedIndex,
      [&visibleSettings](int i) { return std::string(I18N.get(visibleSettings[i].nameId)); }, nullptr, nullptr,
      [&visibleSettings](int i) {
        const auto& setting = visibleSettings[i];
        std::string valueText;
        if (settingShowsNavigationCaret(setting)) {
          valueText = ">";
        } else if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
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
          valueText = formatSettingValue(setting);
        }
        return valueText;
      },
      true, nullptr, [&visibleSettings](int i) { return visibleSettings[i].type == SettingType::SECTION_HEADER; });

  const bool currentIsAction = selectedIndex >= 0 && selectedIndex < settingsCount &&
                               ((*currentSettings)[selectedIndex].type == SettingType::ACTION ||
                                (*currentSettings)[selectedIndex].type == SettingType::SUBMENU ||
                                (*currentSettings)[selectedIndex].nameId == StrId::STR_FONT_FAMILY ||
                                currentSettingUsesOptionMenu((*currentSettings)[selectedIndex]));
  const bool selectedLineHeight = selectedIndex >= 0 && selectedIndex < settingsCount &&
                                  (*currentSettings)[selectedIndex].valuePtr == &CrossPointSettings::lineHeightPercent;
  const bool selectedScreenMargin = selectedIndex >= 0 && selectedIndex < settingsCount &&
                                    (*currentSettings)[selectedIndex].valuePtr == &CrossPointSettings::screenMargin;
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), (currentIsAction || selectedLineHeight || selectedScreenMargin) ? tr(STR_SELECT) : tr(STR_TOGGLE),
      tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
