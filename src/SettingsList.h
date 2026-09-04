#pragma once

#include <CrossInkHalFrontlight.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "QuickActions.h"
#include "activities/settings/SettingsActivity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/FontFamilyLabel.h"
#include "util/FrontlightSchedule.h"

inline std::string fontSizePointLabel(const uint8_t pointSize) { return std::to_string(pointSize) + " pt"; }

inline void appendBuiltinFontSizeOption(SettingInfo& setting, const CrossPointSettings::FONT_SIZE size) {
  const uint8_t pointSize = CrossPointSettings::getReaderFontPointSize(size);
  setting.enumStringValues.push_back(fontSizePointLabel(pointSize));
  setting.enumRawValues.push_back(pointSize);
}

inline SettingInfo buildBuiltinFontSizeSetting() {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::readerFontPointSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.reserve(CrossPointSettings::FONT_SIZE_COUNT);
  s.enumRawValues.reserve(CrossPointSettings::FONT_SIZE_COUNT);

  appendBuiltinFontSizeOption(s, CrossPointSettings::TINY);
  appendBuiltinFontSizeOption(s, CrossPointSettings::SMALL);
  appendBuiltinFontSizeOption(s, CrossPointSettings::MEDIUM);
  appendBuiltinFontSizeOption(s, CrossPointSettings::LARGE);

  return s;
}

inline SettingInfo buildSdFontSizeSetting(const SdCardFontFamilyInfo& family) {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::readerFontPointSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  const std::vector<uint8_t> sizes = family.availableSizes();
  s.enumStringValues.reserve(sizes.size());
  s.enumRawValues.reserve(sizes.size());
  for (size_t i = 0; i < sizes.size(); i++) {
    s.enumStringValues.push_back(fontSizePointLabel(sizes[i]));
    s.enumRawValues.push_back(sizes[i]);
  }
  return s;
}

inline void removeEnumRawValue(SettingInfo& setting, const uint8_t rawValue) {
  const auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return;
  }

  const size_t index = static_cast<size_t>(std::distance(setting.enumRawValues.begin(), it));
  setting.enumRawValues.erase(it);
  if (index < setting.enumValues.size()) {
    setting.enumValues.erase(setting.enumValues.begin() + index);
  }
}

inline bool settingKeyIs(const SettingInfo& setting, const char* key) {
  return setting.key && std::strcmp(setting.key, key) == 0;
}

inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  if (registry && SETTINGS.sdFontFamilyName[0] != '\0') {
    const SdCardFontFamilyInfo* family = registry->findFamily(SETTINGS.sdFontFamilyName);
    if (family && !family->files.empty()) {
      return buildSdFontSizeSetting(*family);
    }
  }
  return buildBuiltinFontSizeSetting();
}

inline uint8_t closestPointSizeIndex(const std::vector<uint8_t>& sizes, const uint8_t targetPointSize) {
  if (sizes.empty()) return 0;

  uint8_t bestIndex = 0;
  uint8_t bestDiff = UINT8_MAX;
  for (size_t i = 0; i < sizes.size(); i++) {
    const uint8_t size = sizes[i];
    const uint8_t diff = size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    if (diff < bestDiff || (diff == bestDiff && size < sizes[bestIndex])) {
      bestIndex = static_cast<uint8_t>(i);
      bestDiff = diff;
    }
  }
  return bestIndex;
}

inline uint8_t closestBuiltinFontSizeIndex(const uint8_t targetPointSize) {
  uint8_t bestStored = 0;
  uint8_t bestPointSize = 0;
  uint8_t bestDiff = UINT8_MAX;

  for (uint8_t i = 0; i < CrossPointSettings::FONT_SIZE_COUNT; i++) {
    const auto size = static_cast<CrossPointSettings::FONT_SIZE>(i);
    const uint8_t stored = CrossPointSettings::getStoredReaderFontSize(size);
    if (stored == UINT8_MAX) continue;

    const uint8_t pointSize = CrossPointSettings::getReaderFontPointSize(size);
    const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
    if (diff < bestDiff || (diff == bestDiff && pointSize < bestPointSize)) {
      bestStored = stored;
      bestPointSize = pointSize;
      bestDiff = diff;
    }
  }
  return bestStored;
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_LEXEND_DECA, StrId::STR_BITTER};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return fontFamilyLabel(f.name, fontFamilyPointSizeRange(f)); });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    constexpr FontFamilyPointSizeRange builtinRange{10, 16};
    allStringValues.push_back(fontFamilyLabel(I18N.get(StrId::STR_LEXEND_DECA), builtinRange));
    allStringValues.push_back(fontFamilyLabel(I18N.get(StrId::STR_BITTER), builtinRange));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  std::vector<std::vector<uint8_t>> sdFamilySizes;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    sdFamilySizes.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilySizes),
                   [](const SdCardFontFamilyInfo& f) { return f.availableSizes(); });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames, sdFamilySizes](uint8_t v) {
    const uint8_t targetPointSize = SETTINGS.readerFontPointSize;

    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.readerFontPointSize = CrossPointSettings::getReaderFontPointSize(
          static_cast<CrossPointSettings::FONT_SIZE>(closestBuiltinFontSizeIndex(targetPointSize)));
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        SETTINGS.readerFontPointSize =
            sdFamilySizes[sdIdx][closestPointSizeIndex(sdFamilySizes[sdIdx], targetPointSize)];
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

inline SettingInfo buildDictionaryFontFamilySetting(const SdCardFontRegistry* registry) {
  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY_FONT;
  s.type = SettingType::ENUM;
  s.key = "dictionaryFont";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.push_back(I18N.get(StrId::STR_USE_READER_FONT));

  std::vector<std::string> familyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    familyNames.reserve(families.size());
    s.enumStringValues.reserve(families.size() + 1);
    for (const auto& family : families) {
      familyNames.push_back(family.name);
      s.enumStringValues.push_back(family.name);
    }
  }

  s.valueGetter = [familyNames]() -> uint8_t {
    for (size_t i = 0; i < familyNames.size(); ++i) {
      if (familyNames[i] == SETTINGS.dictionarySdFontFamilyName) return static_cast<uint8_t>(i + 1);
    }
    return 0;
  };
  s.valueSetter = [familyNames](const uint8_t value) {
    if (value == 0 || value > familyNames.size()) {
      SETTINGS.dictionarySdFontFamilyName[0] = '\0';
      SETTINGS.dictionaryFontPointSize = 0;
      return;
    }
    strncpy(SETTINGS.dictionarySdFontFamilyName, familyNames[value - 1].c_str(),
            sizeof(SETTINGS.dictionarySdFontFamilyName) - 1);
    SETTINGS.dictionarySdFontFamilyName[sizeof(SETTINGS.dictionarySdFontFamilyName) - 1] = '\0';
  };
  return s;
}

inline SettingInfo buildDictionaryFontSizeSetting(const SdCardFontRegistry* registry) {
  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::dictionaryFontPointSize;
  s.key = "dictionaryFontSize";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.push_back(I18N.get(StrId::STR_USE_READER_FONT_SIZE));
  s.enumRawValues.push_back(0);

  if (!registry) return s;
  // With no dedicated dictionary family, a non-zero dictionary size applies
  // to the reader's SD-card family. Built-in reader fonts have no selectable
  // files, so they deliberately retain just the "use reader size" entry.
  const char* familyName =
      SETTINGS.dictionarySdFontFamilyName[0] != '\0' ? SETTINGS.dictionarySdFontFamilyName : SETTINGS.sdFontFamilyName;
  if (familyName[0] == '\0') return s;
  const auto* family = registry->findFamily(familyName);
  if (!family) return s;

  const auto sizes = family->availableSizes();
  s.enumStringValues.reserve(sizes.size() + 1);
  s.enumRawValues.reserve(sizes.size() + 1);
  for (const uint8_t pointSize : sizes) {
    s.enumStringValues.push_back(fontSizePointLabel(pointSize));
    s.enumRawValues.push_back(pointSize);
  }
  return s;
}

inline SettingInfo buildDictionarySetting(const DictionaryRegistry* dictRegistry) {
  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.type = SettingType::ENUM;
  s.key = "dictionary";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.push_back(I18N.get(StrId::STR_DICT_NONE));

  std::vector<DictionaryEntry> entries;
  if (dictRegistry) {
    entries = dictRegistry->getEntries();
    s.enumStringValues.reserve(entries.size() + 1);
    for (const auto& entry : entries) {
      s.enumStringValues.push_back(entry.name);
    }
  }

  s.valueGetter = [entries]() -> uint8_t {
    const std::string activePath = Dictionary::readDictPath();
    if (activePath.empty()) {
      return 0;
    }
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].basePath == activePath) {
        return static_cast<uint8_t>(i + 1);
      }
    }
    return 0;
  };

  s.valueSetter = [entries](uint8_t v) {
    if (v == 0) {
      Dictionary::saveGlobalDictPath("");
      return;
    }
    const size_t entryIndex = static_cast<size_t>(v - 1);
    if (entryIndex < entries.size()) {
      Dictionary::saveGlobalDictPath(entries[entryIndex].basePath.c_str());
    }
  };

  return s;
}

inline SettingInfo buildSleepScreenSetting() {
  SettingInfo s = SettingInfo::Enum(
      StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
      {StrId::STR_NONE_OPT, StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
       StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY, StrId::STR_READING_STATS, StrId::STR_THEME_MINIMAL,
       StrId::STR_THEME_MINIMAL_STATS, StrId::STR_THEME_DASHBOARD, StrId::STR_QUICK_RESUME},
      "sleepScreen", StrId::STR_CAT_DISPLAY);
  s.withEnumRawValues({
      static_cast<uint8_t>(CrossPointSettings::BLANK),
      static_cast<uint8_t>(CrossPointSettings::DARK),
      static_cast<uint8_t>(CrossPointSettings::LIGHT),
      static_cast<uint8_t>(CrossPointSettings::CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::COVER),
      static_cast<uint8_t>(CrossPointSettings::COVER_CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::OVERLAY),
      static_cast<uint8_t>(CrossPointSettings::READING_STATS_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::MINIMAL_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::DASHBOARD_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::QUICK_RESUME),
  });
  return s;
}

enum class ShortcutOptionCatalog { PowerButton, ButtonChord, LongPress, HomeButton };

constexpr uint8_t SHORTCUT_OPTION_UNAVAILABLE = UINT8_MAX;

inline uint8_t shortcutRawValue(const ShortcutOptionCatalog catalog, const CrossPointSettings::SHORT_PWRBTN action) {
  using Action = CrossPointSettings::SHORT_PWRBTN;
  using LongPress = CrossPointSettings::LONG_PRESS_MENU_ACTION;
  using Chord = CrossPointSettings::POWER_CHORD_ACTION;

  switch (catalog) {
    case ShortcutOptionCatalog::PowerButton:
      return static_cast<uint8_t>(action);
    case ShortcutOptionCatalog::ButtonChord:
      switch (action) {
        case Action::IGNORE:
          return Chord::CHORD_DISABLED;
        case Action::SLEEP:
          return Chord::CHORD_SLEEP;
        case Action::PAGE_TURN:
          return Chord::CHORD_PAGE_TURN;
        case Action::PREVIOUS_PAGE:
          return Chord::CHORD_PREVIOUS_PAGE;
        case Action::TOGGLE_BOOKMARK:
          return Chord::CHORD_TOGGLE_BOOKMARK;
        case Action::READING_STATS:
          return Chord::CHORD_READING_STATS;
        case Action::MARK_FINISHED:
          return Chord::CHORD_MARK_FINISHED;
        case Action::FORCE_REFRESH:
          return Chord::CHORD_FORCE_REFRESH;
        case Action::TOGGLE_FONT:
          return Chord::CHORD_TOGGLE_FONT;
        case Action::TOGGLE_GUIDE_DOTS:
          return Chord::CHORD_TOGGLE_GUIDE_DOTS;
        case Action::TOGGLE_BIONIC_READING:
          return Chord::CHORD_TOGGLE_BIONIC_READING;
        case Action::CYCLE_PAGE_TURN:
          return Chord::CHORD_CYCLE_PAGE_TURN;
        case Action::SYNC_PROGRESS:
          return Chord::CHORD_SYNC_PROGRESS;
        case Action::NEARBY_POSITION_SYNC:
          return Chord::CHORD_NEARBY_POSITION_SYNC;
        case Action::FILE_TRANSFER:
          return Chord::CHORD_FILE_TRANSFER;
        case Action::CALIBRE_WIRELESS:
          return Chord::CHORD_CALIBRE_WIRELESS;
        case Action::JOIN_NETWORK:
          return Chord::CHORD_JOIN_NETWORK;
        case Action::CREATE_HOTSPOT:
          return Chord::CHORD_CREATE_HOTSPOT;
        case Action::SCREENSHOT:
          return Chord::CHORD_SCREENSHOT;
        case Action::TOGGLE_DARK_MODE:
          return Chord::CHORD_TOGGLE_DARK_MODE;
        case Action::FOOTNOTES:
          return Chord::CHORD_FOOTNOTES;
        case Action::FILE_BROWSER:
          return Chord::CHORD_FILE_BROWSER;
        case Action::CREATE_CLIPPING:
          return Chord::CHORD_CREATE_CLIPPING;
        case Action::LOOKUP_WORD:
          return Chord::CHORD_LOOKUP_WORD;
        case Action::TOGGLE_HOME_BUTTON_IN_READER:
          return Chord::CHORD_TOGGLE_HOME_BUTTON;
        case Action::QUICK_ACTIONS:
          return Chord::CHORD_QUICK_ACTIONS;
        case Action::TOGGLE_FRONTLIGHT:
          return Chord::CHORD_TOGGLE_FRONTLIGHT;
        case Action::TOGGLE_TOUCHSCREEN:
          return Chord::CHORD_TOGGLE_TOUCHSCREEN;
        case Action::QUICK_LOCK:
          return Chord::CHORD_QUICK_LOCK;
        case Action::TOGGLE_TILT_PAGE_TURN:
          return SHORTCUT_OPTION_UNAVAILABLE;
        default:
          return SHORTCUT_OPTION_UNAVAILABLE;
      }
      break;
    case ShortcutOptionCatalog::LongPress:
      switch (action) {
        case Action::IGNORE:
          return LongPress::LONG_MENU_OFF;
        case Action::SLEEP:
          return LongPress::LONG_MENU_SLEEP;
        case Action::TOGGLE_BOOKMARK:
          return LongPress::LONG_MENU_TOGGLE_BOOKMARK;
        case Action::READING_STATS:
          return LongPress::LONG_MENU_READING_STATS;
        case Action::MARK_FINISHED:
          return LongPress::LONG_MENU_MARK_FINISHED;
        case Action::FORCE_REFRESH:
          return LongPress::LONG_MENU_REFRESH_SCREEN;
        case Action::TOGGLE_FONT:
          return LongPress::LONG_MENU_CHANGE_FONT;
        case Action::TOGGLE_GUIDE_DOTS:
          return LongPress::LONG_MENU_TOGGLE_GUIDE_DOTS;
        case Action::TOGGLE_BIONIC_READING:
          return LongPress::LONG_MENU_TOGGLE_BIONIC;
        case Action::CYCLE_PAGE_TURN:
          return LongPress::LONG_MENU_CYCLE_PAGE_TURN;
        case Action::TOGGLE_TILT_PAGE_TURN:
          return LongPress::LONG_MENU_TOGGLE_TILT_PAGE_TURN;
        case Action::SYNC_PROGRESS:
          return LongPress::LONG_MENU_SYNC_PROGRESS;
        case Action::FILE_TRANSFER:
          return LongPress::LONG_MENU_FILE_TRANSFER;
        case Action::CALIBRE_WIRELESS:
          return LongPress::LONG_MENU_CALIBRE_WIRELESS;
        case Action::JOIN_NETWORK:
          return LongPress::LONG_MENU_JOIN_NETWORK;
        case Action::CREATE_HOTSPOT:
          return LongPress::LONG_MENU_CREATE_HOTSPOT;
        case Action::SCREENSHOT:
          return LongPress::LONG_MENU_SCREENSHOT;
        case Action::TOGGLE_DARK_MODE:
          return LongPress::LONG_MENU_TOGGLE_DARK_MODE;
        case Action::FOOTNOTES:
          return LongPress::LONG_MENU_FOOTNOTES;
        case Action::FILE_BROWSER:
          return LongPress::LONG_MENU_FILE_BROWSER;
        case Action::CREATE_CLIPPING:
          return LongPress::LONG_MENU_CREATE_CLIPPING;
        case Action::LOOKUP_WORD:
          return LongPress::LONG_MENU_LOOKUP_WORD;
        case Action::QUICK_ACTIONS:
          return LongPress::LONG_MENU_QUICK_ACTIONS;
        case Action::QUICK_LOCK:
          return LongPress::LONG_MENU_QUICK_LOCK;
        case Action::PAGE_TURN:
        case Action::PREVIOUS_PAGE:
        case Action::NEARBY_POSITION_SYNC:
        case Action::TOGGLE_HOME_BUTTON_IN_READER:
        case Action::TOGGLE_FRONTLIGHT:
        case Action::TOGGLE_TOUCHSCREEN:
          return SHORTCUT_OPTION_UNAVAILABLE;
        default:
          return SHORTCUT_OPTION_UNAVAILABLE;
      }
      break;
    case ShortcutOptionCatalog::HomeButton:
      switch (action) {
        case Action::SLEEP:
        case Action::TOGGLE_TILT_PAGE_TURN:
        case Action::TOGGLE_HOME_BUTTON_IN_READER:
        case Action::TOGGLE_FRONTLIGHT:
        case Action::TOGGLE_TOUCHSCREEN:
        case Action::QUICK_LOCK:
          return SHORTCUT_OPTION_UNAVAILABLE;
        default:
          return static_cast<uint8_t>(action);
      }
  }

  return SHORTCUT_OPTION_UNAVAILABLE;
}

inline void appendShortcutOptions(SettingInfo& setting, const ShortcutOptionCatalog catalog) {
  setting.enumValues.reserve(QuickActions::shortcutActionOrder.size() + 3);
  setting.enumRawValues.reserve(QuickActions::shortcutActionOrder.size() + 3);

  if (catalog == ShortcutOptionCatalog::HomeButton) {
    setting.enumValues.push_back(StrId::STR_BACK_HOME);
    setting.enumRawValues.push_back(CrossPointSettings::HOME_BUTTON_BACK_HOME);
    if (Frontlight.present()) {
      setting.enumValues.push_back(StrId::STR_TOGGLE_FRONTLIGHT);
      setting.enumRawValues.push_back(CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT);
    }
    setting.enumValues.push_back(StrId::STR_READER_MENU);
    setting.enumRawValues.push_back(CrossPointSettings::HOME_BUTTON_READER_MENU);
  }

  for (const auto action : QuickActions::shortcutActionOrder) {
    if (!QuickActions::isActionAvailable(static_cast<uint8_t>(action))) continue;
    const uint8_t rawValue = shortcutRawValue(catalog, action);
    if (rawValue == SHORTCUT_OPTION_UNAVAILABLE) continue;
    setting.enumValues.push_back(QuickActions::actionLabel(static_cast<uint8_t>(action)));
    setting.enumRawValues.push_back(rawValue);
  }
}

inline SettingInfo buildShortcutSetting(const StrId nameId, uint8_t CrossPointSettings::* const valuePtr,
                                        const char* const key, const ShortcutOptionCatalog catalog) {
  SettingInfo setting = SettingInfo::Enum(nameId, valuePtr, std::vector<StrId>{}, key, StrId::STR_CAT_CONTROLS);
  appendShortcutOptions(setting, catalog);
  return setting;
}

inline SettingInfo buildHomeButtonActionSetting(const StrId nameId, uint8_t CrossPointSettings::* const valuePtr,
                                                const char* const key) {
  return buildShortcutSetting(nameId, valuePtr, key, ShortcutOptionCatalog::HomeButton);
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. Read-only consumers
// can use it directly; mutable device UI lists use getSettingsList(), which
// returns an owned copy and can add SD-card font and dictionary options.
inline const std::vector<SettingInfo>& getBaseSettingsList() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v;
    v.reserve(77);
    auto add = [&v](SettingInfo setting) { v.push_back(std::move(setting)); };

    // --- Display ---
    add(buildSleepScreenSetting());
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                            "quickResumeSleepScreen", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_CLOCK, &CrossPointSettings::hideClock,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideClock",
                          StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::HIDE_CLOCK_NEVER, CrossPointSettings::HIDE_CLOCK_IN_READER,
                                CrossPointSettings::HIDE_CLOCK_ALWAYS}));
    add(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                           StrId::STR_PAGES_30, StrId::STR_NEVER},
                          "refreshFrequency", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::REFRESH_1, CrossPointSettings::REFRESH_5,
                                CrossPointSettings::REFRESH_10, CrossPointSettings::REFRESH_15,
                                CrossPointSettings::REFRESH_30, CrossPointSettings::REFRESH_NEVER}));
    add(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                            StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(
            StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
            {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_MINIMAL, StrId::STR_THEME_DASHBOARD, StrId::STR_THEME_LYRA,
             StrId::STR_THEME_LYRA_EXTENDED, StrId::STR_THEME_LYRA_CAROUSEL, StrId::STR_THEME_ROUNDEDRAFF},
            "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_THEME::CLASSIC, CrossPointSettings::UI_THEME::MINIMAL,
                                CrossPointSettings::UI_THEME::DASHBOARD, CrossPointSettings::UI_THEME::LYRA,
                                CrossPointSettings::UI_THEME::LYRA_3_COVERS,
                                CrossPointSettings::UI_THEME::LYRA_CAROUSEL,
                                CrossPointSettings::UI_THEME::ROUNDEDRAFF}));
    add(SettingInfo::Enum(StrId::STR_UI_SCALE, &CrossPointSettings::uiScale, {StrId::STR_SMALL, StrId::STR_LARGE},
                          "uiScale", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_SCALE_SMALL, CrossPointSettings::UI_SCALE_LARGE}));
    add(SettingInfo::Enum(StrId::STR_RECENT_BOOKS_VIEW, &CrossPointSettings::recentBooksView,
                          {StrId::STR_LIST_VIEW, StrId::STR_GRID_VIEW}, "recentBooksView", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                            "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY));
    // Kept in the shared catalog for persistence and the web API. On-device,
    // these values are presented only by Display > Frontlight.
    add(SettingInfo::Toggle(StrId::STR_FRONTLIGHT_SCHEDULE, &CrossPointSettings::frontlightScheduleEnabled,
                            "frontlightScheduleEnabled", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Value16(StrId::STR_START, &CrossPointSettings::frontlightScheduleStart,
                             {0, FrontlightSchedule::kUnsetTimeOfDay, 1}, "frontlightScheduleStart",
                             StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Value16(StrId::STR_END, &CrossPointSettings::frontlightScheduleEnd,
                             {0, FrontlightSchedule::kUnsetTimeOfDay, 1}, "frontlightScheduleEnd",
                             StrId::STR_CAT_DISPLAY));

    // --- Reader ---
    // Built-in font-family entry. Replaced per-call with a registry-aware
    // version when SD fonts are installed.
    add(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {StrId::STR_LEXEND_DECA, StrId::STR_BITTER}, "fontFamily", StrId::STR_CAT_READER));
    add(buildBuiltinFontSizeSetting());
    add(SettingInfo::Enum(StrId::STR_SD_FONT_SIZE_RANGE, &CrossPointSettings::sdFontSizeRange,
                          {StrId::STR_FONT_RANGE_TEENSY, StrId::STR_FONT_RANGE_TINY, StrId::STR_FONT_RANGE_XLARGE,
                           StrId::STR_FONT_RANGE_ALL},
                          "sdFontSizeRange", StrId::STR_CAT_READER)
            .withEnumRawValues({CrossPointSettings::SD_FONT_RANGE_TEENSY, CrossPointSettings::SD_FONT_RANGE_TINY,
                                CrossPointSettings::SD_FONT_RANGE_XLARGE, CrossPointSettings::SD_FONT_RANGE_ALL}));
    add(SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineHeightPercent,
                           {CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT,
                            CrossPointSettings::LINE_HEIGHT_PERCENT_STEP},
                           "lineHeightPercent", StrId::STR_CAT_READER));
    add(SettingInfo::Value(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                           {0, CrossPointSettings::MAX_WORD_SPACING, 1}, "wordSpacing", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(
            StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
            {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_LANDSCAPE_CCW, StrId::STR_ORIENTATION_INVERTED},
            "orientation", StrId::STR_CAT_READER)
            .withEnumRawValues({CrossPointSettings::PORTRAIT, CrossPointSettings::LANDSCAPE_CW,
                                CrossPointSettings::LANDSCAPE_CCW, CrossPointSettings::INVERTED}));
    add(SettingInfo::Submenu(StrId::STR_SCREEN_MARGIN, SettingAction::ScreenMargin));
    add(SettingInfo::Value(StrId::STR_TOP_BOTTOM, &CrossPointSettings::screenMarginVertical,
                           {CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN,
                            CrossPointSettings::SCREEN_MARGIN_SMALL_STEP},
                           "screenMarginVertical", StrId::STR_CAT_READER));
    add(SettingInfo::Value(StrId::STR_LEFT_RIGHT, &CrossPointSettings::screenMarginHorizontal,
                           {CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN,
                            CrossPointSettings::SCREEN_MARGIN_SMALL_STEP},
                           "screenMarginHorizontal", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_PUBLISHER_PAGE_NUMBERS, &CrossPointSettings::publisherPageNumbers,
                            "publisherPageNumbers", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(
        StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                            "touchReaderControls", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_DISABLE_TOUCHSCREEN, &CrossPointSettings::disableReaderTouchscreen,
                            "disableReaderTouchscreen", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReadingEnabled,
                            "bionicReadingEnabled", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_INDEXING_METHOD, &CrossPointSettings::indexingMethod,
                          {StrId::STR_INDEXING_INCREMENTAL, StrId::STR_INDEXING_FULL_SECTION}, "indexingMethod",
                          StrId::STR_CAT_READER));

    // --- Controls ---
    add(SettingInfo::Toggle(StrId::STR_PINCH_FONT_RESIZE, &CrossPointSettings::pinchFontResizeEnabled,
                            "pinchFontResizeEnabled", StrId::STR_CAT_CONTROLS));
    const std::vector<StrId> twoFingerSwipeActions = {
        StrId::STR_NOT_SET,          StrId::STR_INCREASE_BRIGHTNESS, StrId::STR_DECREASE_BRIGHTNESS,
        StrId::STR_INCREASE_WARMTH,  StrId::STR_DECREASE_WARMTH,     StrId::STR_NEXT_CHAPTER,
        StrId::STR_PREVIOUS_CHAPTER, StrId::STR_INCREASE_FONT_SIZE,  StrId::STR_DECREASE_FONT_SIZE,
    };
    const std::vector<uint8_t> twoFingerSwipeActionValues = {
        CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET,
        CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS,
        CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS,
        CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH,
        CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH,
        CrossPointSettings::TWO_FINGER_SWIPE_NEXT_CHAPTER,
        CrossPointSettings::TWO_FINGER_SWIPE_PREVIOUS_CHAPTER,
        CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE,
        CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE,
    };
    add(SettingInfo::Enum(StrId::STR_TWO_FINGER_SWIPE_UP, &CrossPointSettings::twoFingerSwipeUp, twoFingerSwipeActions,
                          "twoFingerSwipeUp", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_TWO_FINGER_SWIPE_DOWN, &CrossPointSettings::twoFingerSwipeDown,
                          twoFingerSwipeActions, "twoFingerSwipeDown", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_TWO_FINGER_SWIPE_LEFT, &CrossPointSettings::twoFingerSwipeLeft,
                          twoFingerSwipeActions, "twoFingerSwipeLeft", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_TWO_FINGER_SWIPE_RIGHT, &CrossPointSettings::twoFingerSwipeRight,
                          twoFingerSwipeActions, "twoFingerSwipeRight", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_DISABLED, StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_NEXT_NEXT},
                          "sideButtonLayout", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::SIDE_BUTTONS_DISABLED, CrossPointSettings::PREV_NEXT,
                                CrossPointSettings::NEXT_PREV, CrossPointSettings::NEXT_NEXT}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::sideButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_YES}, "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::sideButtonLongPress,
                          {StrId::STR_IGNORE, StrId::STR_CHAPTER_SKIP_OPT, StrId::STR_CHANGE_FONT_SIZE,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "sideButtonLongPress", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::SIDE_LONG_OFF, CrossPointSettings::SIDE_LONG_CHAPTER_SKIP,
                                CrossPointSettings::SIDE_LONG_FONT_SIZE,
                                CrossPointSettings::SIDE_LONG_ORIENTATION_CHANGE}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_CHANGE_FONT_SIZE, StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::OFF, CrossPointSettings::CHAPTER_SKIP,
                                CrossPointSettings::FONT_SIZE_CHANGE, CrossPointSettings::ORIENTATION_CHANGE}));
    add(buildShortcutSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn, "shortPwrBtn",
                             ShortcutOptionCatalog::PowerButton));
    add(buildShortcutSetting(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPwrBtn, "longPwrBtn",
                             ShortcutOptionCatalog::PowerButton));
    add(buildShortcutSetting(StrId::STR_POWER_BUTTON_CHORD, &CrossPointSettings::powerChordAction, "powerChordAction",
                             ShortcutOptionCatalog::ButtonChord));
    add(buildShortcutSetting(StrId::STR_SIDE_BUTTON_CHORD, &CrossPointSettings::sideButtonChordAction,
                             "sideButtonChordAction", ShortcutOptionCatalog::ButtonChord));
    add(SettingInfo::Enum(StrId::STR_IN_READER, &CrossPointSettings::homeButtonInReaderEnabled,
                          {StrId::STR_ENABLED, StrId::STR_DISABLED}, "homeButtonInReaderEnabled",
                          StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({1, 0}));
    add(buildHomeButtonActionSetting(StrId::STR_HOME_BUTTON_TAP, &CrossPointSettings::homeButtonTapAction,
                                     "homeButtonTapAction"));
    add(buildHomeButtonActionSetting(StrId::STR_HOME_BUTTON_DOUBLE_TAP, &CrossPointSettings::homeButtonDoubleTapAction,
                                     "homeButtonDoubleTapAction"));
    add(buildHomeButtonActionSetting(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::homeButtonLongPressAction,
                                     "homeButtonLongPressAction"));
    add(buildShortcutSetting(StrId::STR_LONG_PRESS_MENU_ACTION, &CrossPointSettings::longPressMenuAction,
                             "longPressMenuAction", ShortcutOptionCatalog::LongPress));
    add(buildShortcutSetting(StrId::STR_LONG_PRESS_BACK_ACTION, &CrossPointSettings::longPressBackAction,
                             "longPressBackAction", ShortcutOptionCatalog::LongPress));
    add(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_PAGE_TURN, &CrossPointSettings::pageTurnGesture,
                          {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                           StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                          "pageTurnGesture", StrId::STR_CAT_CONTROLS));

    // --- System ---
    add(SettingInfo::String(StrId::STR_DEVICE_NAME, SETTINGS.deviceName, sizeof(SETTINGS.deviceName), "deviceName",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_HIDE_FILE_EXTENSION, &CrossPointSettings::hideFileExtension, "hideFileExtension",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Enum(StrId::STR_FILE_BROWSER_DISPLAY, &CrossPointSettings::fileBrowserDisplay,
                          {StrId::STR_FILE_BROWSER_DISPLAY_1_LINE, StrId::STR_FILE_BROWSER_DISPLAY_2_LINES},
                          "fileBrowserDisplay", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_AUTO_BACKUP_STATS, &CrossPointSettings::autoBackupStats, "autoBackupStats",
                            StrId::STR_CAT_SYSTEM));
    // Persisted and available to the web settings API, but category-less because
    // the on-device editor lives under System > OPDS Servers.
    add(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, SETTINGS.opdsDownloadFolder,
                            sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
    // Persisted here, but edited from the nearby receive screen's folder picker.
    add(SettingInfo::String(StrId::STR_NEARBY_RECEIVE_FOLDER, SETTINGS.nearbyReceiveFolder,
                            sizeof(SETTINGS.nearbyReceiveFolder), "nearbyReceiveFolder"));
    add(SettingInfo::Value(StrId::STR_IDLE_TIME_THRESHOLD, &CrossPointSettings::readingIdleTimeThresholdUnits,
                           {CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_UNITS,
                            CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_UNITS, 1},
                           "readingIdleTimeThresholdUnits", StrId::STR_CAT_SYSTEM));
#ifdef CROSSINK_ENABLE_READING_STATS_TOGGLE
    add(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats, "trackReadingStats",
                            StrId::STR_CAT_SYSTEM));
#endif

    // Frontlight quick-panel state: persisted + web-exposed, category-less so
    // it stays off the Settings screen (edited from the swipe-down panel).
    add(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                           "frontlightBrightness"));
    add(SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
    add(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& v) {
          KOREADER_STORE.setServerUrl(v);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        [](uint8_t v) {
          KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
          KOREADER_STORE.saveToFile();
        },
        "koMatchMethod", StrId::STR_KOREADER_SYNC));
    add(SettingInfo::DynamicEnum(
        StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
        [](uint8_t v) {
          KOREADER_STORE.setSendMetadata(v != 0);
          KOREADER_STORE.saveToFile();
        },
        "koSendMetadata", StrId::STR_KOREADER_SYNC));

    add(SettingInfo::DynamicEnum(
        StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
        [](uint8_t v) {
          KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
          KOREADER_STORE.saveToFile();
        },
        "koSyncBehavior", StrId::STR_KOREADER_SYNC));

    // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
    add(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_STABLE_PAGE_NUMBERS, &CrossPointSettings::stablePageNumbers, "stablePageNumbers",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
            .withEnumRawValues({CrossPointSettings::HIDE_PROGRESS, CrossPointSettings::BOOK_PROGRESS,
                                CrossPointSettings::CHAPTER_PROGRESS}));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR)
            .withEnumRawValues(
                {CrossPointSettings::HIDE_TITLE, CrossPointSettings::BOOK_TITLE, CrossPointSettings::CHAPTER_TITLE}));
    add(SettingInfo::Enum(StrId::STR_TIME_LEFT, &CrossPointSettings::statusBarTimeLeft,
                          {StrId::STR_HIDE, StrId::STR_CHAPTER, StrId::STR_BOOK}, "statusBarTimeLeft",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Clock detail entries live under System > Device in the device UI.
    // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
    add(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Enum(StrId::STR_DATE_FORMAT, &CrossPointSettings::dateFormat,
                          {StrId::STR_DATE_FORMAT_MONTH_DAY_YEAR_LONG, StrId::STR_DATE_FORMAT_DAY_MONTH_YEAR_LONG,
                           StrId::STR_DATE_FORMAT_MONTH_DAY_YEAR_NUMERIC, StrId::STR_DATE_FORMAT_DAY_MONTH_YEAR_NUMERIC,
                           StrId::STR_DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC, StrId::STR_DATE_FORMAT_MONTH_DAY_NUMERIC,
                           StrId::STR_DATE_FORMAT_DAY_MONTH_NUMERIC, StrId::STR_DATE_FORMAT_MONTH_DAY_LONG,
                           StrId::STR_DATE_FORMAT_DAY_MONTH_LONG},
                          "dateFormat", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Enum(
        StrId::STR_DATE_SEPARATOR, &CrossPointSettings::dateSeparator,
        {StrId::STR_DATE_SEPARATOR_PERIOD, StrId::STR_DATE_SEPARATOR_HYPHEN, StrId::STR_DATE_SEPARATOR_SLASH},
        "dateSeparator", StrId::STR_CAT_SYSTEM));
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    add(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CAT_SYSTEM));
    // Only show tilt page turn settings when the active device has a supported IMU.
    if (QuickActions::supportsTiltPageTurn()) {
      auto shortPowerButtonIt = std::find_if(
          v.begin(), v.end(), [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SHORT_PWR_BTN; });
      if (shortPowerButtonIt != v.end()) {
        auto insertPos = v.insert(shortPowerButtonIt + 1,
                                  SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                                      "tiltPageTurn", StrId::STR_CAT_CONTROLS));
        v.insert(
            insertPos + 1,
            SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN_DIRECTION, &CrossPointSettings::tiltPageTurnDirection,
                              {StrId::STR_TILT_DIRECTION_LEFT_RIGHT, StrId::STR_TILT_DIRECTION_LEFT_RIGHT_INVERTED,
                               StrId::STR_TILT_DIRECTION_FORWARD_BACK, StrId::STR_TILT_DIRECTION_FORWARD_BACK_INVERTED},
                              "tiltPageTurnDirection", StrId::STR_CAT_CONTROLS));
      }
    } else {
      for (auto& setting : v) {
        if (setting.nameId == StrId::STR_SHORT_PWR_BTN || settingKeyIs(setting, "longPwrBtn")) {
          removeEnumRawValue(setting, static_cast<uint8_t>(CrossPointSettings::TOGGLE_TILT_PAGE_TURN));
        } else if (setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION ||
                   setting.nameId == StrId::STR_LONG_PRESS_BACK_ACTION) {
          removeEnumRawValue(setting, static_cast<uint8_t>(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN));
        }
      }
    }

    if (!gpio.deviceIsX3()) {
      auto sleepScreenIt =
          std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_SLEEP_SCREEN; });
      if (sleepScreenIt != v.end()) {
        removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP));
      }
    }
    return v;
  }();

  return baseList;
}

inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const DictionaryRegistry* dictRegistry = nullptr) {
  std::vector<SettingInfo> v = getBaseSettingsList();
  const bool hasTouch = gpio.hasTouch();
  if (!hasTouch) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_TOUCH_READER_CONTROLS ||
                                    s.nameId == StrId::STR_DISABLE_TOUCHSCREEN || s.nameId == StrId::STR_PAGE_TURN ||
                                    s.nameId == StrId::STR_PINCH_FONT_RESIZE ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_UP ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT;
                           }),
            v.end());
  }
  if (!gpio.supportsMultiTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_PINCH_FONT_RESIZE ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_UP ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT ||
                                    s.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT;
                           }),
            v.end());
  }
  for (auto& setting : v) {
    const bool isTwoFingerSwipe =
        setting.nameId == StrId::STR_TWO_FINGER_SWIPE_UP || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
        setting.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT;
    if (!isTwoFingerSwipe) continue;
    if (!Frontlight.present()) {
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS);
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS);
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH);
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH);
    } else if (!Frontlight.hasColorTemperature()) {
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH);
      removeEnumRawValue(setting, CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH);
    }
  }
  if (hasTouch) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION ||
                                    s.nameId == StrId::STR_SUNLIGHT_FADING_FIX;
                           }),
            v.end());

    const auto themeIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_UI_THEME; });
    if (themeIt != v.end()) {
      removeEnumRawValue(*themeIt, static_cast<uint8_t>(CrossPointSettings::UI_THEME::CLASSIC));
      removeEnumRawValue(*themeIt, static_cast<uint8_t>(CrossPointSettings::UI_THEME::ROUNDEDRAFF));
    }
  }
  if (!gpio.hasHomeKey()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_IN_READER || s.nameId == StrId::STR_HOME_BUTTON_TAP ||
                                    s.nameId == StrId::STR_HOME_BUTTON_DOUBLE_TAP ||
                                    settingKeyIs(s, "homeButtonLongPressAction");
                           }),
            v.end());
    for (auto& setting : v) {
      if (setting.nameId == StrId::STR_SHORT_PWR_BTN || settingKeyIs(setting, "longPwrBtn") ||
          settingKeyIs(setting, "powerChordAction") || settingKeyIs(setting, "sideButtonChordAction")) {
        removeEnumRawValue(setting, CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER);
      }
    }
  }
  if (!Frontlight.present() || !gpio.hasTouch()) {
    for (auto& setting : v) {
      if (setting.nameId != StrId::STR_SHORT_PWR_BTN && !settingKeyIs(setting, "longPwrBtn") &&
          !settingKeyIs(setting, "powerChordAction") && !settingKeyIs(setting, "sideButtonChordAction")) {
        continue;
      }
      if (!Frontlight.present()) removeEnumRawValue(setting, CrossPointSettings::TOGGLE_FRONTLIGHT);
      if (!gpio.hasTouch()) removeEnumRawValue(setting, CrossPointSettings::TOGGLE_TOUCHSCREEN);
    }
  }
  if (!Frontlight.present()) {
    for (auto& setting : v) {
      if (setting.nameId == StrId::STR_REFRESH_FREQ) {
        removeEnumRawValue(setting, CrossPointSettings::REFRESH_NEVER);
      }
    }
  }
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
    auto fontSizeIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (fontSizeIt != v.end()) {
      *fontSizeIt = buildFontSizeSetting(registry);
    }
  }
  if (dictRegistry) {
    if (dictRegistry->count() > 0) {
      auto fontSizeIt =
          std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
      const size_t insertIndex =
          fontSizeIt == v.end() ? v.size() : static_cast<size_t>(std::distance(v.begin(), fontSizeIt) + 1);
      v.insert(v.begin() + insertIndex, buildDictionaryFontFamilySetting(registry));
      v.insert(v.begin() + insertIndex + 1, buildDictionaryFontSizeSetting(registry));
    }
    auto guideIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_GUIDE_READING; });
    const auto insertPos = guideIt == v.end() ? v.end() : guideIt + 1;
    v.insert(insertPos, buildDictionarySetting(dictRegistry));
  }
  return v;
}

inline void addSettingByName(std::vector<SettingInfo>& target, const std::vector<SettingInfo>& allSettings,
                             StrId nameId) {
  const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                               [nameId](const auto& setting) { return setting.nameId == nameId; });
  if (it != allSettings.end()) {
    target.push_back(*it);
  }
}

inline std::vector<SettingInfo> buildReaderSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> readerSettings;
  readerSettings.reserve(12);
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_FONT_OPTIONS, SettingAction::ReaderFontOptions));
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_PAGE_LAYOUT, SettingAction::ReaderPageLayout));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  addSettingByName(readerSettings, allSettings, StrId::STR_PUBLISHER_PAGE_NUMBERS);
  addSettingByName(readerSettings, allSettings, StrId::STR_DISABLE_TOUCHSCREEN);
  addSettingByName(readerSettings, allSettings, StrId::STR_EMBEDDED_STYLE);
  addSettingByName(readerSettings, allSettings, StrId::STR_IMAGES);
  addSettingByName(readerSettings, allSettings, StrId::STR_BIONIC_READING);
  addSettingByName(readerSettings, allSettings, StrId::STR_GUIDE_READING);
  addSettingByName(readerSettings, allSettings, StrId::STR_DICTIONARY);
  addSettingByName(readerSettings, allSettings, StrId::STR_INDEXING_METHOD);
  return readerSettings;
}

inline std::vector<SettingInfo> buildBookReaderSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  auto settings = buildReaderSettingsParentList(allSettings);
  settings.erase(
      std::remove_if(settings.begin(), settings.end(),
                     [](const SettingInfo& setting) { return setting.nameId == StrId::STR_DISABLE_TOUCHSCREEN; }),
      settings.end());
  return settings;
}

inline std::vector<SettingInfo> buildReaderFontSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(10);
  addSettingByName(settings, allSettings, StrId::STR_FONT_FAMILY);
  addSettingByName(settings, allSettings, StrId::STR_FONT_SIZE);
  addSettingByName(settings, allSettings, StrId::STR_DICTIONARY_FONT);
  addSettingByName(settings, allSettings, StrId::STR_DICTIONARY_FONT_SIZE);
  addSettingByName(settings, allSettings, StrId::STR_LINE_SPACING);
  addSettingByName(settings, allSettings, StrId::STR_WORD_SPACING);
  addSettingByName(settings, allSettings, StrId::STR_TEXT_AA);
  settings.push_back(SettingInfo::Action(StrId::STR_DOWNLOAD_FONTS, SettingAction::DownloadFonts));
  addSettingByName(settings, allSettings, StrId::STR_SD_FONT_SIZE_RANGE);
  return settings;
}

inline std::vector<SettingInfo> buildReaderPageLayoutSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  addSettingByName(settings, allSettings, StrId::STR_ORIENTATION);
  addSettingByName(settings, allSettings, StrId::STR_SCREEN_MARGIN);
  addSettingByName(settings, allSettings, StrId::STR_PARA_ALIGNMENT);
  addSettingByName(settings, allSettings, StrId::STR_HYPHENATION);
  addSettingByName(settings, allSettings, StrId::STR_EXTRA_SPACING);
  addSettingByName(settings, allSettings, StrId::STR_FORCE_PARAGRAPH_INDENTS);
  return settings;
}

inline std::vector<SettingInfo> buildReaderScreenMarginSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(2);
  addSettingByName(settings, allSettings, StrId::STR_TOP_BOTTOM);
  addSettingByName(settings, allSettings, StrId::STR_LEFT_RIGHT);
  return settings;
}

inline void addSettingByKey(std::vector<SettingInfo>& target, const std::vector<SettingInfo>& allSettings,
                            const char* key) {
  const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                               [key](const auto& setting) { return settingKeyIs(setting, key); });
  if (it != allSettings.end()) {
    target.push_back(*it);
  }
}

inline bool hasSettingByName(const std::vector<SettingInfo>& allSettings, StrId nameId) {
  return std::any_of(allSettings.begin(), allSettings.end(),
                     [nameId](const auto& setting) { return setting.nameId == nameId; });
}

inline std::vector<SettingInfo> buildControlsSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  const bool hasTiltPageTurnSetting = hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN);
  const bool hasTiltPageTurnDirectionSetting = hasSettingByName(allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION);
  const bool hasTapsGestures = hasSettingByName(allSettings, StrId::STR_PAGE_TURN);
  const bool hasFrontButtons = !gpio.hasTouch();
  const bool hasHomeKey = gpio.hasHomeKey();

  std::vector<SettingInfo> settings;
  settings.reserve(3 + (hasHomeKey ? 1u : 0u) + (hasFrontButtons ? 1u : 0u) + (hasTiltPageTurnSetting ? 1u : 0u) +
                   (hasTiltPageTurnDirectionSetting ? 1u : 0u) + (hasTapsGestures ? 1u : 0u));
  if (hasHomeKey) {
    settings.push_back(SettingInfo::Submenu(StrId::STR_HOME_BUTTON, SettingAction::ControlsHomeButton));
  }
  settings.push_back(SettingInfo::Submenu(StrId::STR_POWER_BUTTON, SettingAction::ControlsPowerButton));
  if (hasFrontButtons) {
    settings.push_back(SettingInfo::Submenu(StrId::STR_FRONT_BUTTONS, SettingAction::ControlsFrontButtons));
  }
  settings.push_back(SettingInfo::Submenu(StrId::STR_SIDE_BUTTONS, SettingAction::ControlsSideButtons));
  settings.push_back(SettingInfo::Action(StrId::STR_QUICK_ACTIONS, SettingAction::QuickActions));
  if (hasTapsGestures) {
    settings.push_back(SettingInfo::Submenu(StrId::STR_TAPS_AND_GESTURES, SettingAction::ControlsTapsGestures));
  }
  if (hasTiltPageTurnSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN);
  if (hasTiltPageTurnDirectionSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsTapsGesturesSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  const bool hasPinch = hasSettingByName(allSettings, StrId::STR_PINCH_FONT_RESIZE);
  const bool hasTwoFingerSwipe = hasSettingByName(allSettings, StrId::STR_TWO_FINGER_SWIPE_UP);
  settings.reserve(1 + (hasPinch ? 1u : 0u) + (hasTwoFingerSwipe ? 1u : 0u));
  addSettingByName(settings, allSettings, StrId::STR_PAGE_TURN);
  if (hasPinch) addSettingByName(settings, allSettings, StrId::STR_PINCH_FONT_RESIZE);
  if (hasTwoFingerSwipe) {
    settings.push_back(SettingInfo::Submenu(StrId::STR_TWO_FINGER_SWIPE, SettingAction::ControlsTwoFingerSwipe));
  }
  return settings;
}

inline std::vector<SettingInfo> buildControlsTwoFingerSwipeSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(4);
  addSettingByName(settings, allSettings, StrId::STR_TWO_FINGER_SWIPE_UP);
  addSettingByName(settings, allSettings, StrId::STR_TWO_FINGER_SWIPE_DOWN);
  addSettingByName(settings, allSettings, StrId::STR_TWO_FINGER_SWIPE_LEFT);
  addSettingByName(settings, allSettings, StrId::STR_TWO_FINGER_SWIPE_RIGHT);
  return settings;
}

inline std::vector<SettingInfo> buildControlsHomeButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(4);
  addSettingByName(settings, allSettings, StrId::STR_IN_READER);
  addSettingByName(settings, allSettings, StrId::STR_HOME_BUTTON_TAP);
  addSettingByName(settings, allSettings, StrId::STR_HOME_BUTTON_DOUBLE_TAP);
  addSettingByKey(settings, allSettings, "homeButtonLongPressAction");
  return settings;
}

inline std::vector<SettingInfo> buildControlsPowerSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(4);
  addSettingByName(settings, allSettings, StrId::STR_SHORT_PWR_BTN);
  addSettingByKey(settings, allSettings, "longPwrBtn");
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPressMenuAction == CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES ||
      SETTINGS.longPressBackAction == CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES) {
    addSettingByName(settings, allSettings, StrId::STR_PWR_BTN_FOOTNOTE_BACK);
  }
  addSettingByName(settings, allSettings, StrId::STR_POWER_BUTTON_CHORD);
  return settings;
}

inline std::vector<SettingInfo> buildControlsFrontButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  settings.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  settings.push_back(
      SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS_READER, SettingAction::RemapFrontButtonsReader));
  addSettingByKey(settings, allSettings, "frontButtonOrientationAware");
  addSettingByKey(settings, allSettings, "longPressButtonBehavior");
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_BACK_ACTION);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_MENU_ACTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsSideButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3 + (gpio.hasTouch() ? 1u : 0u));
  addSettingByName(settings, allSettings, StrId::STR_SIDE_BTN_LAYOUT);
  addSettingByKey(settings, allSettings, "sideButtonOrientationAware");
  addSettingByKey(settings, allSettings, "sideButtonLongPress");
  if (gpio.hasTouch()) {
    addSettingByName(settings, allSettings, StrId::STR_SIDE_BUTTON_CHORD);
  }
  return settings;
}

inline std::vector<SettingInfo> buildGroupedDisplaySettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> displaySettings;
  displaySettings.reserve(9);

  auto addDisplaySetting = [&](StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      displaySettings.push_back(*it);
    }
  };

  displaySettings.push_back(SettingInfo::Submenu(StrId::STR_DISPLAY_SLEEP_SCREEN, SettingAction::DisplaySleepScreen));
  if (Frontlight.present()) {
    displaySettings.push_back(SettingInfo::Submenu(StrId::STR_FRONTLIGHT, SettingAction::DisplayFrontlight));
  }
  addDisplaySetting(StrId::STR_HIDE_BATTERY);
  if (halClock.isAvailable()) {
    addDisplaySetting(StrId::STR_HIDE_CLOCK);
  }
  addDisplaySetting(StrId::STR_REFRESH_FREQ);
  addDisplaySetting(StrId::STR_NIGHT_MODE);
  addDisplaySetting(StrId::STR_UI_THEME);
  addDisplaySetting(StrId::STR_UI_SCALE);
  addDisplaySetting(StrId::STR_RECENT_BOOKS_VIEW);
  addDisplaySetting(StrId::STR_SUNLIGHT_FADING_FIX);

  return displaySettings;
}

inline std::vector<SettingInfo> buildDisplayFrontlightSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(4);

  auto addDisplaySetting = [&](const StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) settings.push_back(*it);
  };

  addDisplaySetting(StrId::STR_RESTORE_LIGHT_ON_WAKE);
  if (halClock.isAvailable()) {
    addDisplaySetting(StrId::STR_FRONTLIGHT_SCHEDULE);
    addDisplaySetting(StrId::STR_START);
    addDisplaySetting(StrId::STR_END);
  }
  return settings;
}

inline std::vector<SettingInfo> buildDisplaySleepSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> sleepSettings;
  sleepSettings.reserve(4);

  auto addSleepSetting = [&](StrId nameId, StrId displayNameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      sleepSettings.push_back(*it);
      sleepSettings.back().nameId = displayNameId;
    }
  };

  addSleepSetting(StrId::STR_SLEEP_SCREEN, StrId::STR_SLEEP_SCREEN_WALLPAPER);
  addSleepSetting(StrId::STR_SLEEP_COVER_MODE, StrId::STR_SLEEP_COVER_MODE_SHORT);
  addSleepSetting(StrId::STR_SLEEP_COVER_FILTER, StrId::STR_SLEEP_COVER_FILTER_SHORT);
  addSleepSetting(StrId::STR_QUICK_RESUME_TIMEOUT, StrId::STR_QUICK_RESUME_TIMEOUT);

  return sleepSettings;
}

inline std::vector<SettingInfo> buildSystemSettingsParentList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> systemSettings;
  systemSettings.reserve(8);
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_SYSTEM_DEVICE, SettingAction::SystemDevice));
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_SYSTEM_FILES_CACHE, SettingAction::SystemFilesCache));
  systemSettings.push_back(SettingInfo::Submenu(StrId::STR_READING_STATS, SettingAction::SystemReadingStats));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  return systemSettings;
}

inline std::vector<SettingInfo> buildSystemDeviceSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(9);
  addSettingByName(settings, allSettings, StrId::STR_DEVICE_NAME);
  addSettingByName(settings, allSettings, StrId::STR_TIME_TO_SLEEP);
  settings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  settings.push_back(SettingInfo::Action(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts));
  if (halClock.isAvailable()) {
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_FORMAT);
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_UTC_OFFSET);
    addSettingByName(settings, allSettings, StrId::STR_DATE_FORMAT);
    addSettingByName(settings, allSettings, StrId::STR_DATE_SEPARATOR);
    settings.push_back(SettingInfo::Action(StrId::STR_CLOCK_SYNC_NOW, SettingAction::ClockSync));
  }
  return settings;
}

inline std::vector<SettingInfo> buildSystemFilesCacheSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  addSettingByName(settings, allSettings, StrId::STR_SHOW_HIDDEN_FILES);
  addSettingByName(settings, allSettings, StrId::STR_HIDE_FILE_EXTENSION);
  addSettingByName(settings, allSettings, StrId::STR_FILE_BROWSER_DISPLAY);
  addSettingByName(settings, allSettings, StrId::STR_REMOVE_READ_FROM_RECENTS);
  addSettingByName(settings, allSettings, StrId::STR_MOVE_FINISHED_TO_READ);
  settings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  return settings;
}

inline std::vector<SettingInfo> buildFileBrowserSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_SHOW_HIDDEN_FILES);
  addSettingByName(settings, allSettings, StrId::STR_HIDE_FILE_EXTENSION);
  addSettingByName(settings, allSettings, StrId::STR_FILE_BROWSER_DISPLAY);
  return settings;
}

inline std::vector<SettingInfo> buildSystemReadingStatsSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_TRACK_READING_STATS);
  settings.push_back(SettingInfo::Submenu(StrId::STR_ALL_TIME_STATS, SettingAction::SystemGlobalStats));
  addSettingByName(settings, allSettings, StrId::STR_IDLE_TIME_THRESHOLD);
  return settings;
}

inline std::vector<SettingInfo> buildSystemGlobalStatsSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  if (halClock.isAvailable()) {
    addSettingByName(settings, allSettings, StrId::STR_AUTO_BACKUP_STATS);
  }
  settings.push_back(SettingInfo::Action(StrId::STR_BACKUP_NOW, SettingAction::BackupStats));
  settings.push_back(SettingInfo::Action(StrId::STR_RESET_ALL_TIME_STATS, SettingAction::ResetGlobalStats));
  return settings;
}
