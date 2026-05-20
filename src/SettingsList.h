#pragma once

#include <HalClock.h>
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

inline StrId fontSizeLabelForPointSize(const uint8_t pointSize) {
  switch (pointSize) {
    case 8:
      return StrId::STR_TEENSY;
    case 9:
      return StrId::STR_ITTY_BITTY;
    case 10:
      return StrId::STR_TINY;
    case 12:
      return StrId::STR_SMALL;
    case 14:
      return StrId::STR_MEDIUM;
    case 16:
      return StrId::STR_LARGE;
    case 18:
      return StrId::STR_X_LARGE;
    case 20:
      return StrId::STR_HUGE;
    default:
      return StrId::STR_NONE_OPT;
  }
}

inline SettingInfo buildBuiltinFontSizeSetting() {
  return SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                           {
#ifndef OMIT_TINY_FONT
                               StrId::STR_TINY,
#endif
#ifndef OMIT_SMALL_FONT
                               StrId::STR_SMALL,
#endif
#ifndef OMIT_MEDIUM_FONT
                               StrId::STR_MEDIUM,
#endif
#ifndef OMIT_LARGE_FONT
                               StrId::STR_LARGE,
#endif
#ifndef OMIT_XLARGE_FONT
                               StrId::STR_X_LARGE,
#endif
#ifndef OMIT_TEENSY_FONT
                               StrId::STR_TEENSY,
#endif
#ifndef OMIT_HUGE_FONT
                               StrId::STR_HUGE,
#endif
#ifndef OMIT_ITTY_BITTY_FONT
                               StrId::STR_ITTY_BITTY,
#endif
                           },
                           "fontSize", StrId::STR_CAT_READER);
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
    const StrId labelId = fontSizeLabelForPointSize(sizes[i]);
    s.enumStringValues.push_back(labelId != StrId::STR_NONE_OPT ? I18N.get(labelId) : std::to_string(sizes[i]) + " pt");
    s.enumRawValues.push_back(static_cast<uint8_t>(i));
  }
  return s;
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
  SettingInfo s = SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                                    {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER,
                                     StrId::STR_NONE_OPT, StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY,
                                     StrId::STR_READING_STATS, StrId::STR_THEME_MINIMAL, StrId::STR_QUICK_RESUME},
                                    "sleepScreen", StrId::STR_CAT_DISPLAY);
  s.withEnumRawValues({
      static_cast<uint8_t>(CrossPointSettings::DARK),
      static_cast<uint8_t>(CrossPointSettings::LIGHT),
      static_cast<uint8_t>(CrossPointSettings::CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::COVER),
      static_cast<uint8_t>(CrossPointSettings::BLANK),
      static_cast<uint8_t>(CrossPointSettings::COVER_CUSTOM),
      static_cast<uint8_t>(CrossPointSettings::OVERLAY),
      static_cast<uint8_t>(CrossPointSettings::READING_STATS_SLEEP),
      static_cast<uint8_t>(CrossPointSettings::MINIMAL_SLEEP),
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
    v.reserve(64);
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
    add(SettingInfo::Enum(
        StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
        {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
        "refreshFrequency", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                           StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_LYRA_CAROUSEL, StrId::STR_THEME_MINIMAL},
                          "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_THEME::CLASSIC, CrossPointSettings::UI_THEME::LYRA,
                                CrossPointSettings::UI_THEME::LYRA_3_COVERS, CrossPointSettings::UI_THEME::ROUNDEDRAFF,
                                CrossPointSettings::UI_THEME::LYRA_CAROUSEL, CrossPointSettings::UI_THEME::MINIMAL}));
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
    add(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing",
                          StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                          "orientation", StrId::STR_CAT_READER));
    add(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER));
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
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::sideButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_YES}, "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_SIDE_BTN_LONG_PRESS, &CrossPointSettings::sideButtonLongPress,
                          {StrId::STR_CHAPTER_SKIP_OPT, StrId::STR_CHANGE_FONT_SIZE, StrId::STR_IGNORE,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "sideButtonLongPress", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                          {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                           StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                          "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(
        StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT,
         StrId::STR_TOGGLE_GUIDE_DOTS, StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK,
         StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS, StrId::STR_SCREENSHOT_BUTTON,
         StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER},
        "shortPwrBtn", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(
        StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::longPwrBtn,
        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_CHANGE_FONT,
         StrId::STR_TOGGLE_GUIDE_DOTS, StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK,
         StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS, StrId::STR_SCREENSHOT_BUTTON,
         StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER},
        "longPwrBtn", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_LONG_PRESS_MENU_ACTION, &CrossPointSettings::longPressMenuAction,
                          {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_CHANGE_FONT, StrId::STR_TOGGLE_GUIDE_DOTS,
                           StrId::STR_TOGGLE_BIONIC_READING, StrId::STR_TOGGLE_BOOKMARK, StrId::STR_FORCE_REFRESH,
                           StrId::STR_SYNC_PROGRESS, StrId::STR_MARK_FINISHED, StrId::STR_READING_STATS,
                           StrId::STR_SCREENSHOT_BUTTON, StrId::STR_CYCLE_PAGE_TURN, StrId::STR_FILE_TRANSFER},
                          "longPressMenuAction", StrId::STR_CAT_CONTROLS));

    // --- System ---
    add(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                            "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                            "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));

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
    add(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
    // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
    add(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR));
    add(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
    // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
    // on next WiFi connect, which is useful when crossing time zones.
    add(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CUSTOMISE_STATUS_BAR));
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3).
    if (halTiltSensor.isAvailable()) {
      for (auto& setting : v) {
        if (setting.nameId == StrId::STR_SHORT_PWR_BTN || setting.nameId == StrId::STR_LONG_PRESS_ACTION ||
            setting.nameId == StrId::STR_LONG_PRESS_MENU_ACTION) {
          setting.enumValues.push_back(StrId::STR_TILT_PAGE_TURN);
        }
      }
      const auto shortPowerIt = std::find_if(
          v.begin(), v.end(), [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SHORT_PWR_BTN; });
      if (shortPowerIt != v.end()) {
        v.insert(shortPowerIt + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                                     {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                                     "tiltPageTurn", StrId::STR_CAT_CONTROLS));
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
  return v;
}
