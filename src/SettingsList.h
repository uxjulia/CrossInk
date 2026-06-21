#pragma once

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
#include "activities/settings/SettingsActivity.h"

inline std::string fontSizePointLabel(const uint8_t pointSize) { return std::to_string(pointSize) + " pt"; }

inline void appendBuiltinFontSizeOption(SettingInfo& setting, const CrossPointSettings::FONT_SIZE size) {
  const uint8_t stored = CrossPointSettings::getStoredReaderFontSize(size);
  if (stored == UINT8_MAX) return;

  setting.enumStringValues.push_back(fontSizePointLabel(CrossPointSettings::getReaderFontPointSize(size)));
  setting.enumRawValues.push_back(stored);
}

inline SettingInfo buildBuiltinFontSizeSetting() {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.enumStringValues.reserve(CrossPointSettings::FONT_SIZE_COUNT);
  s.enumRawValues.reserve(CrossPointSettings::FONT_SIZE_COUNT);

  appendBuiltinFontSizeOption(s, CrossPointSettings::TEENSY);
  appendBuiltinFontSizeOption(s, CrossPointSettings::ITTY_BITTY);
  appendBuiltinFontSizeOption(s, CrossPointSettings::TINY);
  appendBuiltinFontSizeOption(s, CrossPointSettings::SMALL);
  appendBuiltinFontSizeOption(s, CrossPointSettings::MEDIUM);
  appendBuiltinFontSizeOption(s, CrossPointSettings::LARGE);
  appendBuiltinFontSizeOption(s, CrossPointSettings::EXTRA_LARGE);
  appendBuiltinFontSizeOption(s, CrossPointSettings::HUGE_SIZE);

  return s;
}

inline SettingInfo buildSdFontSizeSetting(const SdCardFontFamilyInfo& family) {
  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.valuePtr = &CrossPointSettings::fontSize;
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;

  const std::vector<uint8_t> sizes = family.availableSizes();
  s.enumStringValues.reserve(sizes.size());
  s.enumRawValues.reserve(sizes.size());
  for (size_t i = 0; i < sizes.size(); i++) {
    s.enumStringValues.push_back(fontSizePointLabel(sizes[i]));
    s.enumRawValues.push_back(static_cast<uint8_t>(i));
  }
  return s;
}

inline void insertEnumOptionAfter(SettingInfo& setting, const StrId after, const StrId option, const uint8_t rawValue) {
  const auto it = std::find(setting.enumValues.begin(), setting.enumValues.end(), after);
  if (it == setting.enumValues.end()) {
    setting.enumValues.push_back(option);
    if (!setting.enumRawValues.empty()) setting.enumRawValues.push_back(rawValue);
    return;
  }

  const auto insertIndex = static_cast<size_t>(std::distance(setting.enumValues.begin(), it) + 1);
  setting.enumValues.insert(it + 1, option);
  if (!setting.enumRawValues.empty()) {
    setting.enumRawValues.insert(setting.enumRawValues.begin() + insertIndex, rawValue);
  }
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
  std::vector<StrId> enumValues = {StrId::STR_LEXEND_DECA, StrId::STR_BITTER, StrId::STR_CHAREINK};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_LEXEND_DECA));
    allStringValues.push_back(I18N.get(StrId::STR_BITTER));
    allStringValues.push_back(I18N.get(StrId::STR_CHAREINK));
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
    uint8_t targetPointSize = CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (size_t i = 0; i < sdFamilyNames.size(); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName && SETTINGS.fontSize < sdFamilySizes[i].size()) {
          targetPointSize = sdFamilySizes[i][SETTINGS.fontSize];
          break;
        }
      }
    }

    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.fontSize = closestBuiltinFontSizeIndex(targetPointSize);
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        SETTINGS.fontSize = closestPointSizeIndex(sdFamilySizes[sdIdx], targetPointSize);
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

inline SettingInfo buildSleepScreenSetting() {
  SettingInfo s =
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_NONE_OPT, StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
                         StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY, StrId::STR_READING_STATS,
                         StrId::STR_THEME_MINIMAL, StrId::STR_THEME_MINIMAL_STATS, StrId::STR_QUICK_RESUME},
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
      static_cast<uint8_t>(CrossPointSettings::QUICK_RESUME),
  });
  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The static list is constructed exactly once (master's optimization, #1086 +
// #1636) so the per-entry SettingInfo cost is paid once. When an
// SdCardFontRegistry is supplied AND has SD card fonts installed, the
// font-family entry is replaced in a per-call copy with a registry-aware
// version. Callers without SD fonts pay only a vector copy.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v;
    v.reserve(66);
    auto add = [&v](SettingInfo setting) { v.push_back(std::move(setting)); };

    // --- Display ---
    add(buildSleepScreenSetting());
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                          {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_HIDE_CLOCK, &CrossPointSettings::hideClock,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideClock",
                          StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::HIDE_CLOCK_NEVER, CrossPointSettings::HIDE_CLOCK_IN_READER,
                                CrossPointSettings::HIDE_CLOCK_ALWAYS}));
    add(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(
            StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
            {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_MINIMAL, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
             StrId::STR_THEME_LYRA_CAROUSEL, StrId::STR_THEME_ROUNDEDRAFF},
            "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_THEME::CLASSIC, CrossPointSettings::UI_THEME::MINIMAL,
                                CrossPointSettings::UI_THEME::LYRA, CrossPointSettings::UI_THEME::LYRA_3_COVERS,
                                CrossPointSettings::UI_THEME::LYRA_CAROUSEL,
                                CrossPointSettings::UI_THEME::ROUNDEDRAFF}));
    add(SettingInfo::Enum(StrId::STR_RECENT_BOOKS_VIEW, &CrossPointSettings::recentBooksView,
                          {StrId::STR_LIST_VIEW, StrId::STR_GRID_VIEW}, "recentBooksView", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY));

    // --- Reader ---
    // Built-in font-family entry. Replaced per-call with a registry-aware
    // version when SD fonts are installed.
    add(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {StrId::STR_LEXEND_DECA, StrId::STR_BITTER, StrId::STR_CHAREINK}, "fontFamily",
                          StrId::STR_CAT_READER));
    add(buildBuiltinFontSizeSetting());
    add(SettingInfo::Enum(StrId::STR_SD_FONT_SIZE_RANGE, &CrossPointSettings::sdFontSizeRange,
                          {StrId::STR_FONT_RANGE_TEENSY, StrId::STR_FONT_RANGE_TINY, StrId::STR_FONT_RANGE_XLARGE,
                           StrId::STR_FONT_RANGE_NO_EMOJI, StrId::STR_FONT_RANGE_ALL},
                          "sdFontSizeRange", StrId::STR_CAT_READER));
    add(SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineHeightPercent,
                           {CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT,
                            CrossPointSettings::LINE_HEIGHT_PERCENT_STEP},
                           "lineHeightPercent", StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_LANDSCAPE_CCW, StrId::STR_INVERTED},
                          "orientation", StrId::STR_CAT_READER)
            .withEnumRawValues({CrossPointSettings::PORTRAIT, CrossPointSettings::LANDSCAPE_CW,
                                CrossPointSettings::LANDSCAPE_CCW, CrossPointSettings::INVERTED}));
    add(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER));
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
    add(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode, "readerDarkMode",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReadingEnabled,
                            "bionicReadingEnabled", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER));

    // --- Controls ---
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_DISABLED, StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_NEXT_NEXT},
                          "sideButtonLayout", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::SIDE_BUTTONS_DISABLED, CrossPointSettings::PREV_NEXT,
                                CrossPointSettings::NEXT_PREV, CrossPointSettings::NEXT_NEXT}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::sideButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_YES}, "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LONG_PRESS, &CrossPointSettings::sideButtonLongPress,
                          {StrId::STR_IGNORE, StrId::STR_CHAPTER_SKIP_OPT, StrId::STR_CHANGE_FONT_SIZE,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "sideButtonLongPress", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::SIDE_LONG_OFF, CrossPointSettings::SIDE_LONG_CHAPTER_SKIP,
                                CrossPointSettings::SIDE_LONG_FONT_SIZE,
                                CrossPointSettings::SIDE_LONG_ORIENTATION_CHANGE}));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_CHANGE_FONT_SIZE, StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::OFF, CrossPointSettings::CHAPTER_SKIP,
                                CrossPointSettings::FONT_SIZE_CHANGE, CrossPointSettings::ORIENTATION_CHANGE}));
    add(SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                          {StrId::STR_IGNORE,
                           StrId::STR_SLEEP,
                           StrId::STR_PAGE_TURN,
                           StrId::STR_TOGGLE_BOOKMARK,
                           StrId::STR_READING_STATS,
                           StrId::STR_MARK_FINISHED,
                           StrId::STR_FORCE_REFRESH,
                           StrId::STR_CHANGE_FONT,
                           StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING,
                           StrId::STR_CYCLE_PAGE_TURN,
                           StrId::STR_SYNC_PROGRESS,
                           StrId::STR_FILE_TRANSFER,
                           StrId::STR_CALIBRE_WIRELESS,
                           StrId::STR_JOIN_NETWORK,
                           StrId::STR_CREATE_HOTSPOT,
                           StrId::STR_SCREENSHOT_BUTTON,
                           StrId::STR_READER_DARK_MODE,
                           StrId::STR_FOOTNOTES,
                           StrId::STR_BROWSE_FILES},
                          "shortPwrBtn", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::IGNORE,
                                CrossPointSettings::SLEEP,
                                CrossPointSettings::PAGE_TURN,
                                CrossPointSettings::TOGGLE_BOOKMARK,
                                CrossPointSettings::READING_STATS,
                                CrossPointSettings::MARK_FINISHED,
                                CrossPointSettings::FORCE_REFRESH,
                                CrossPointSettings::TOGGLE_FONT,
                                CrossPointSettings::TOGGLE_GUIDE_DOTS,
                                CrossPointSettings::TOGGLE_BIONIC_READING,
                                CrossPointSettings::CYCLE_PAGE_TURN,
                                CrossPointSettings::SYNC_PROGRESS,
                                CrossPointSettings::FILE_TRANSFER,
                                CrossPointSettings::CALIBRE_WIRELESS,
                                CrossPointSettings::JOIN_NETWORK,
                                CrossPointSettings::CREATE_HOTSPOT,
                                CrossPointSettings::SCREENSHOT,
                                CrossPointSettings::TOGGLE_DARK_MODE,
                                CrossPointSettings::FOOTNOTES,
                                CrossPointSettings::FILE_BROWSER}));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPwrBtn,
                          {StrId::STR_IGNORE,
                           StrId::STR_SLEEP,
                           StrId::STR_PAGE_TURN,
                           StrId::STR_TOGGLE_BOOKMARK,
                           StrId::STR_READING_STATS,
                           StrId::STR_MARK_FINISHED,
                           StrId::STR_FORCE_REFRESH,
                           StrId::STR_CHANGE_FONT,
                           StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING,
                           StrId::STR_CYCLE_PAGE_TURN,
                           StrId::STR_SYNC_PROGRESS,
                           StrId::STR_FILE_TRANSFER,
                           StrId::STR_CALIBRE_WIRELESS,
                           StrId::STR_JOIN_NETWORK,
                           StrId::STR_CREATE_HOTSPOT,
                           StrId::STR_SCREENSHOT_BUTTON,
                           StrId::STR_READER_DARK_MODE,
                           StrId::STR_FOOTNOTES,
                           StrId::STR_BROWSE_FILES},
                          "longPwrBtn", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues({CrossPointSettings::IGNORE,
                                CrossPointSettings::SLEEP,
                                CrossPointSettings::PAGE_TURN,
                                CrossPointSettings::TOGGLE_BOOKMARK,
                                CrossPointSettings::READING_STATS,
                                CrossPointSettings::MARK_FINISHED,
                                CrossPointSettings::FORCE_REFRESH,
                                CrossPointSettings::TOGGLE_FONT,
                                CrossPointSettings::TOGGLE_GUIDE_DOTS,
                                CrossPointSettings::TOGGLE_BIONIC_READING,
                                CrossPointSettings::CYCLE_PAGE_TURN,
                                CrossPointSettings::SYNC_PROGRESS,
                                CrossPointSettings::FILE_TRANSFER,
                                CrossPointSettings::CALIBRE_WIRELESS,
                                CrossPointSettings::JOIN_NETWORK,
                                CrossPointSettings::CREATE_HOTSPOT,
                                CrossPointSettings::SCREENSHOT,
                                CrossPointSettings::TOGGLE_DARK_MODE,
                                CrossPointSettings::FOOTNOTES,
                                CrossPointSettings::FILE_BROWSER}));
    add(SettingInfo::Enum(
            StrId::STR_LONG_PRESS_MENU_ACTION, &CrossPointSettings::longPressMenuAction,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_READING_STATS,
             StrId::STR_MARK_FINISHED, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
             StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_SYNC_PROGRESS,
             StrId::STR_FILE_TRANSFER, StrId::STR_CALIBRE_WIRELESS, StrId::STR_JOIN_NETWORK, StrId::STR_CREATE_HOTSPOT,
             StrId::STR_SCREENSHOT_BUTTON, StrId::STR_READER_DARK_MODE, StrId::STR_FOOTNOTES, StrId::STR_BROWSE_FILES},
            "longPressMenuAction", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(
                {CrossPointSettings::LONG_MENU_OFF, CrossPointSettings::LONG_MENU_SLEEP,
                 CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK, CrossPointSettings::LONG_MENU_READING_STATS,
                 CrossPointSettings::LONG_MENU_MARK_FINISHED, CrossPointSettings::LONG_MENU_REFRESH_SCREEN,
                 CrossPointSettings::LONG_MENU_CHANGE_FONT, CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS,
                 CrossPointSettings::LONG_MENU_TOGGLE_BIONIC, CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN,
                 CrossPointSettings::LONG_MENU_SYNC_PROGRESS, CrossPointSettings::LONG_MENU_FILE_TRANSFER,
                 CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS, CrossPointSettings::LONG_MENU_JOIN_NETWORK,
                 CrossPointSettings::LONG_MENU_CREATE_HOTSPOT, CrossPointSettings::LONG_MENU_SCREENSHOT,
                 CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE, CrossPointSettings::LONG_MENU_FOOTNOTES,
                 CrossPointSettings::LONG_MENU_FILE_BROWSER}));
    add(SettingInfo::Enum(
            StrId::STR_LONG_PRESS_BACK_ACTION, &CrossPointSettings::longPressBackAction,
            {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_READING_STATS,
             StrId::STR_MARK_FINISHED, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
             StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_SYNC_PROGRESS,
             StrId::STR_FILE_TRANSFER, StrId::STR_CALIBRE_WIRELESS, StrId::STR_JOIN_NETWORK, StrId::STR_CREATE_HOTSPOT,
             StrId::STR_SCREENSHOT_BUTTON, StrId::STR_READER_DARK_MODE, StrId::STR_FOOTNOTES, StrId::STR_BROWSE_FILES},
            "longPressBackAction", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(
                {CrossPointSettings::LONG_MENU_OFF, CrossPointSettings::LONG_MENU_SLEEP,
                 CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK, CrossPointSettings::LONG_MENU_READING_STATS,
                 CrossPointSettings::LONG_MENU_MARK_FINISHED, CrossPointSettings::LONG_MENU_REFRESH_SCREEN,
                 CrossPointSettings::LONG_MENU_CHANGE_FONT, CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS,
                 CrossPointSettings::LONG_MENU_TOGGLE_BIONIC, CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN,
                 CrossPointSettings::LONG_MENU_SYNC_PROGRESS, CrossPointSettings::LONG_MENU_FILE_TRANSFER,
                 CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS, CrossPointSettings::LONG_MENU_JOIN_NETWORK,
                 CrossPointSettings::LONG_MENU_CREATE_HOTSPOT, CrossPointSettings::LONG_MENU_SCREENSHOT,
                 CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE, CrossPointSettings::LONG_MENU_FOOTNOTES,
                 CrossPointSettings::LONG_MENU_FILE_BROWSER}));
    add(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));

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
    add(SettingInfo::Value(StrId::STR_IDLE_TIME_THRESHOLD, &CrossPointSettings::readingIdleTimeThresholdUnits,
                           {CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_UNITS,
                            CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_UNITS, 1},
                           "readingIdleTimeThresholdUnits", StrId::STR_CAT_SYSTEM));
#ifdef CROSSINK_ENABLE_READING_STATS_TOGGLE
    add(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats, "trackReadingStats",
                            StrId::STR_CAT_SYSTEM));
#endif

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
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    add(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CAT_SYSTEM));
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3).
    if (halTiltSensor.isAvailable()) {
      for (auto& setting : v) {
        if (setting.nameId == StrId::STR_SHORT_PWR_BTN || setting.nameId == StrId::STR_LONG_PRESS_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_BACK_ACTION) {
          const uint8_t rawValue =
              setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION || setting.nameId == StrId::STR_LONG_PRESS_BACK_ACTION
                  ? static_cast<uint8_t>(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN)
                  : static_cast<uint8_t>(CrossPointSettings::TOGGLE_TILT_PAGE_TURN);
          insertEnumOptionAfter(setting, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_TILT_PAGE_TURN, rawValue);
        }
      }
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
    }
    return v;
  }();

  std::vector<SettingInfo> v = baseList;
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
  if (!gpio.deviceIsX3()) {
    auto sleepScreenIt =
        std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_SLEEP_SCREEN; });
    if (sleepScreenIt != v.end()) {
      removeEnumRawValue(*sleepScreenIt, static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP));
    }
  }
  return v;
}

inline std::vector<SettingInfo> buildGroupedReaderSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> readerSettings;
  readerSettings.reserve(22);

  auto addReaderSetting = [&](StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      readerSettings.push_back(*it);
    }
  };

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_FONT_OPTIONS));
  addReaderSetting(StrId::STR_FONT_FAMILY);
  addReaderSetting(StrId::STR_FONT_SIZE);
  readerSettings.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  addReaderSetting(StrId::STR_SD_FONT_SIZE_RANGE);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_PAGE_LAYOUT));
  addReaderSetting(StrId::STR_LINE_SPACING);
  addReaderSetting(StrId::STR_SCREEN_MARGIN);
  addReaderSetting(StrId::STR_PARA_ALIGNMENT);
  addReaderSetting(StrId::STR_EXTRA_SPACING);
  addReaderSetting(StrId::STR_FORCE_PARAGRAPH_INDENTS);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_BOOK_STYLING));
  addReaderSetting(StrId::STR_EMBEDDED_STYLE);
  addReaderSetting(StrId::STR_HYPHENATION);
  addReaderSetting(StrId::STR_TEXT_AA);
  addReaderSetting(StrId::STR_IMAGES);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_READING_AIDS));
  addReaderSetting(StrId::STR_BIONIC_READING);
  addReaderSetting(StrId::STR_GUIDE_READING);

  readerSettings.push_back(SettingInfo::SectionHeader(StrId::STR_READER_UI));
  addReaderSetting(StrId::STR_ORIENTATION);
  addReaderSetting(StrId::STR_PUBLISHER_PAGE_NUMBERS);
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  return readerSettings;
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
  readerSettings.reserve(8);
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_FONT_OPTIONS, SettingAction::ReaderFontOptions));
  readerSettings.push_back(SettingInfo::Submenu(StrId::STR_READER_PAGE_LAYOUT, SettingAction::ReaderPageLayout));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  addSettingByName(readerSettings, allSettings, StrId::STR_PUBLISHER_PAGE_NUMBERS);
  addSettingByName(readerSettings, allSettings, StrId::STR_READER_DARK_MODE);
  addSettingByName(readerSettings, allSettings, StrId::STR_EMBEDDED_STYLE);
  addSettingByName(readerSettings, allSettings, StrId::STR_IMAGES);
  addSettingByName(readerSettings, allSettings, StrId::STR_BIONIC_READING);
  addSettingByName(readerSettings, allSettings, StrId::STR_GUIDE_READING);
  return readerSettings;
}

inline std::vector<SettingInfo> buildReaderFontSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(5);
  addSettingByName(settings, allSettings, StrId::STR_FONT_FAMILY);
  addSettingByName(settings, allSettings, StrId::STR_FONT_SIZE);
  addSettingByName(settings, allSettings, StrId::STR_LINE_SPACING);
  settings.push_back(SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  addSettingByName(settings, allSettings, StrId::STR_SD_FONT_SIZE_RANGE);
  addSettingByName(settings, allSettings, StrId::STR_TEXT_AA);
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

inline void addSettingByKey(std::vector<SettingInfo>& target, const std::vector<SettingInfo>& allSettings,
                            const char* key) {
  const auto it = std::find_if(allSettings.begin(), allSettings.end(), [key](const auto& setting) {
    return setting.key && std::strcmp(setting.key, key) == 0;
  });
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

  std::vector<SettingInfo> settings;
  settings.reserve(3 + (hasTiltPageTurnSetting ? 1u : 0u) + (hasTiltPageTurnDirectionSetting ? 1u : 0u));
  settings.push_back(SettingInfo::Submenu(StrId::STR_POWER_BUTTON, SettingAction::ControlsPowerButton));
  settings.push_back(SettingInfo::Submenu(StrId::STR_FRONT_BUTTONS, SettingAction::ControlsFrontButtons));
  settings.push_back(SettingInfo::Submenu(StrId::STR_SIDE_BUTTONS, SettingAction::ControlsSideButtons));
  if (hasTiltPageTurnSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN);
  if (hasTiltPageTurnDirectionSetting) addSettingByName(settings, allSettings, StrId::STR_TILT_PAGE_TURN_DIRECTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsPowerSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_SHORT_PWR_BTN);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_ACTION);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES ||
      SETTINGS.longPressMenuAction == CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES ||
      SETTINGS.longPressBackAction == CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FOOTNOTES) {
    addSettingByName(settings, allSettings, StrId::STR_PWR_BTN_FOOTNOTE_BACK);
  }
  return settings;
}

inline std::vector<SettingInfo> buildControlsFrontButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(6);
  settings.push_back(SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  settings.push_back(
      SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS_READER, SettingAction::RemapFrontButtonsReader));
  addSettingByKey(settings, allSettings, "frontButtonOrientationAware");
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_BEHAVIOR);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_BACK_ACTION);
  addSettingByName(settings, allSettings, StrId::STR_LONG_PRESS_MENU_ACTION);
  return settings;
}

inline std::vector<SettingInfo> buildControlsSideButtonSettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> settings;
  settings.reserve(3);
  addSettingByName(settings, allSettings, StrId::STR_SIDE_BTN_LAYOUT);
  addSettingByKey(settings, allSettings, "sideButtonOrientationAware");
  addSettingByName(settings, allSettings, StrId::STR_SIDE_BTN_LONG_PRESS);
  return settings;
}

inline std::vector<SettingInfo> buildGroupedDisplaySettingsList(const std::vector<SettingInfo>& allSettings) {
  std::vector<SettingInfo> displaySettings;
  displaySettings.reserve(7);

  auto addDisplaySetting = [&](StrId nameId) {
    const auto it = std::find_if(allSettings.begin(), allSettings.end(),
                                 [nameId](const auto& setting) { return setting.nameId == nameId; });
    if (it != allSettings.end()) {
      displaySettings.push_back(*it);
    }
  };

  displaySettings.push_back(SettingInfo::Submenu(StrId::STR_DISPLAY_SLEEP_SCREEN, SettingAction::DisplaySleepScreen));
  addDisplaySetting(StrId::STR_HIDE_BATTERY);
  if (halClock.isAvailable()) {
    addDisplaySetting(StrId::STR_HIDE_CLOCK);
  }
  addDisplaySetting(StrId::STR_REFRESH_FREQ);
  addDisplaySetting(StrId::STR_UI_THEME);
  addDisplaySetting(StrId::STR_RECENT_BOOKS_VIEW);
  addDisplaySetting(StrId::STR_SUNLIGHT_FADING_FIX);

  return displaySettings;
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
  settings.reserve(7);
  addSettingByName(settings, allSettings, StrId::STR_DEVICE_NAME);
  addSettingByName(settings, allSettings, StrId::STR_TIME_TO_SLEEP);
  settings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  if (halClock.isAvailable()) {
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_FORMAT);
    addSettingByName(settings, allSettings, StrId::STR_CLOCK_UTC_OFFSET);
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
