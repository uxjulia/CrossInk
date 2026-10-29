#include "CrossPointSettings.h"

#include <BoardConfig.h>
#include <CrossInkHalFrontlight.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>
#include <ScalableBuiltins.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>

#include "I18nKeys.h"
#include "QuickActions.h"
#include "SettingsList.h"
#include "fontIds.h"
#include "util/FrontlightSchedule.h"
#include "util/PluginLocations.h"
#include "util/ReaderStatusBarJson.h"
#include "util/TwoFingerSwipe.h"

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 2;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/crossink-settings.json";
constexpr char SETTINGS_FILE_JSON_BAK[] = "/.crosspoint/crossink-settings.json.bak";
constexpr char LEGACY_SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";
constexpr uint8_t INVALID_READER_FONT_SIZE = 0xFF;
constexpr uint8_t TILT_DIRECTION_SCHEMA_CURRENT = 2;
// X3 hardware predates this migration by less than a year. Reject the RTC's
// factory/default year while preserving dates written by released firmware.
constexpr uint16_t MIN_TRUSTED_MIGRATED_RTC_YEAR = 2025;
constexpr uint8_t SLEEP_SCREEN_STORAGE_ORDER[] = {
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
    static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::DASHBOARD_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::COVER_OVERLAY),
};
constexpr uint8_t SLEEP_SCREEN_STORAGE_ORDER_COUNT =
    sizeof(SLEEP_SCREEN_STORAGE_ORDER) / sizeof(SLEEP_SCREEN_STORAGE_ORDER[0]);
static_assert(SLEEP_SCREEN_STORAGE_ORDER_COUNT == CrossPointSettings::SLEEP_SCREEN_MODE_COUNT,
              "Update sleep screen persisted-value mapping when adding modes");
constexpr CrossPointSettings::FONT_SIZE READER_FONT_SIZE_STORAGE_ORDER[] = {
    CrossPointSettings::TINY,
    CrossPointSettings::SMALL,
    CrossPointSettings::MEDIUM,
    CrossPointSettings::LARGE,
};
constexpr uint8_t SD_FONT_RANGE_POINT_SIZES[CrossPointSettings::SD_FONT_SIZE_RANGE_COUNT]
                                           [CrossPointSettings::SD_FONT_MAX_SIZE_STEPS] = {
                                               {8, 9, 10, 12},
                                               {10, 12, 14, 16},
                                               {14, 16, 18, 20},
                                               {8, 9, 10, 12, 14, 16, 18, 20},
                                               {8, 9, 10, 12, 14, 16, 18, 20},
};
constexpr uint8_t SD_FONT_RANGE_STEP_COUNTS[CrossPointSettings::SD_FONT_SIZE_RANGE_COUNT] = {4, 4, 4, 8, 8};

bool isValidDeviceName(const char* name) {
  if (!name) return false;
  const size_t len = std::strlen(name);
  return len >= CrossPointSettings::MIN_DEVICE_NAME_LENGTH && len <= CrossPointSettings::MAX_DEVICE_NAME_LENGTH;
}

bool restoreLegacyRtcDateSyncState(CrossPointSettings& settings) {
  if (!settings.clockHasBeenSynced || settings.clockDateHasBeenSynced || !halClock.isAvailable()) return false;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute) || year < MIN_TRUSTED_MIGRATED_RTC_YEAR) return false;

  settings.clockDateHasBeenSynced = 1;
  LOG_INF("CPS", "Restored RTC date sync state from valid persisted date: %04u-%02u-%02u", static_cast<unsigned>(year),
          static_cast<unsigned>(month), static_cast<unsigned>(day));
  return true;
}

uint8_t normalizedSdFontRange(uint8_t range) {
  if (range == CrossPointSettings::SD_FONT_RANGE_NO_EMOJI_LEGACY) {
    return CrossPointSettings::SD_FONT_RANGE_ALL;
  }
  return range < CrossPointSettings::SD_FONT_SIZE_RANGE_COUNT ? range : CrossPointSettings::SD_FONT_RANGE_TINY;
}

bool isReaderFontSizeAvailable(const CrossPointSettings::FONT_SIZE size) {
  return size < CrossPointSettings::FONT_SIZE_COUNT;
}

CrossPointSettings::FONT_SIZE firstAvailableReaderFontSize() {
  const auto it =
      std::find_if(std::begin(READER_FONT_SIZE_STORAGE_ORDER), std::end(READER_FONT_SIZE_STORAGE_ORDER),
                   [](const CrossPointSettings::FONT_SIZE size) { return isReaderFontSizeAvailable(size); });
  return (it != std::end(READER_FONT_SIZE_STORAGE_ORDER)) ? *it : CrossPointSettings::TINY;
}

#if !CROSSINK_SCALABLE_FONTS
int getFallbackReaderFontIdForFamily(const CrossPointSettings::FONT_FAMILY family) {
  switch (family) {
    case CrossPointSettings::BITTER:
      return BITTER_10_FONT_ID;
    case CrossPointSettings::LEXENDDECA:
    default:
      return LEXENDDECA_10_FONT_ID;
  }
}

#endif

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

// Hide Battery % (Never / In Reader / Always) predates per-bar battery styles.
// In Reader hid the percentage only in reader bars, so the UI header kept it.
ReaderStatusBarBatteryStyle legacyBatteryStyle(const uint8_t hideBatteryPercentage, const bool displayBar) {
  const bool hidden = displayBar ? hideBatteryPercentage == CrossPointSettings::HIDE_ALWAYS
                                 : hideBatteryPercentage != CrossPointSettings::HIDE_NEVER;
  return hidden ? ReaderStatusBarBatteryStyle::IconOnly : ReaderStatusBarBatteryStyle::IconAndPercent;
}

void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

bool isEnumRawValueAllowed(const SettingInfo& info, const uint8_t value) {
  if (info.enumRawValues.empty()) return value < settingEnumOptionCount(info);
  return std::find(info.enumRawValues.begin(), info.enumRawValues.end(), value) != info.enumRawValues.end();
}

uint8_t defaultEnumRawValue(const SettingInfo& info, const uint8_t fieldDefault) {
  if (isEnumRawValueAllowed(info, fieldDefault)) return fieldDefault;
  return info.enumRawValues.empty() ? 0 : info.enumRawValues.front();
}

bool isSleepScreenSetting(const SettingInfo& info) { return info.key && strcmp(info.key, "sleepScreen") == 0; }

bool isValidQuickActionSlot(const uint8_t action) {
  return action < CrossPointSettings::QUICK_ACTION_SLOT_ACTION_COUNT ||
         action == CrossPointSettings::TOGGLE_HOME_BUTTON_IN_READER ||
         action == CrossPointSettings::TOGGLE_FRONTLIGHT || action == CrossPointSettings::TOGGLE_TOUCHSCREEN ||
         action == CrossPointSettings::PREVIOUS_PAGE || action == CrossPointSettings::NEARBY_POSITION_SYNC ||
         action == CrossPointSettings::LIBRARY || action == CrossPointSettings::HOME_READER ||
         action == CrossPointSettings::BACK_HOME || action == CrossPointSettings::SELECT_CHAPTER ||
         action == CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES || action == CrossPointSettings::ROTATE_FLIP;
}

uint8_t migrateTiltDirectionValue(const uint8_t direction) {
  if (direction == CrossPointSettings::TILT_LEFT_RIGHT) return CrossPointSettings::TILT_LEFT_RIGHT_INVERTED;
  if (direction == CrossPointSettings::TILT_LEFT_RIGHT_INVERTED) return CrossPointSettings::TILT_LEFT_RIGHT;
  return direction;
}

}  // namespace

const char* CrossPointSettings::getDefaultDeviceName() {
#if (defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC) || defined(SIMULATOR_DEVICE_X4_CLASSIC)
  return "X4 Classic";
#endif
  if (BoardConfig::isSticky()) return "Sticky";
  if (BoardConfig::isX4Pro()) return "CrossInk X4 Pro";
  if (gpio.deviceIsX3()) return "CrossInk X3";
  if (gpio.deviceIsX4()) return "CrossInk X4";
  return "CrossInk";
}

const char* CrossPointSettings::getEffectiveDeviceName() const {
  return isValidDeviceName(deviceName) ? deviceName : getDefaultDeviceName();
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

bool CrossPointSettings::isTwoFingerSwipeActionAvailable(const uint8_t action, const bool frontlightPresent,
                                                         const bool hasColorTemperature) {
  switch (static_cast<TWO_FINGER_SWIPE_ACTION>(action)) {
    case TWO_FINGER_SWIPE_NOT_SET:
    case TWO_FINGER_SWIPE_BACK_HOME:
    case TWO_FINGER_SWIPE_HOME_READER:
    case TWO_FINGER_SWIPE_SELECT_CHAPTER:
    case TWO_FINGER_SWIPE_TOGGLE_PAGE_TURN_GESTURES:
    case TWO_FINGER_SWIPE_NEXT_CHAPTER:
    case TWO_FINGER_SWIPE_PREVIOUS_CHAPTER:
    case TWO_FINGER_SWIPE_INCREASE_FONT_SIZE:
    case TWO_FINGER_SWIPE_DECREASE_FONT_SIZE:
      return true;
    case TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS:
    case TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS:
      return frontlightPresent;
    case TWO_FINGER_SWIPE_INCREASE_WARMTH:
    case TWO_FINGER_SWIPE_DECREASE_WARMTH:
      return frontlightPresent && hasColorTemperature;
    case TWO_FINGER_SWIPE_ACTION_COUNT:
      return false;
  }
  return false;
}

bool CrossPointSettings::normalizeTwoFingerSwipeActions(CrossPointSettings& settings,
                                                        uint8_t CrossPointSettings::* const editedField) {
  uint8_t CrossPointSettings::* const fields[] = {
      &CrossPointSettings::twoFingerSwipeUp, &CrossPointSettings::twoFingerSwipeDown,
      &CrossPointSettings::twoFingerSwipeLeft, &CrossPointSettings::twoFingerSwipeRight};
  bool changed = false;
  const bool frontlightPresent = Frontlight.present();
  const bool hasColorTemperature = Frontlight.hasColorTemperature();

  for (const auto field : fields) {
    uint8_t& action = settings.*field;
    if (!isTwoFingerSwipeActionAvailable(action, frontlightPresent, hasColorTemperature)) {
      action = TWO_FINGER_SWIPE_NOT_SET;
      changed = true;
    }
  }

  uint8_t actions[] = {settings.twoFingerSwipeUp, settings.twoFingerSwipeDown, settings.twoFingerSwipeLeft,
                       settings.twoFingerSwipeRight};
  int editedIndex = -1;
  for (int i = 0; i < 4; i++) {
    if (fields[i] == editedField) editedIndex = i;
  }
  changed = TwoFingerSwipe::clearDuplicateActions(actions, TWO_FINGER_SWIPE_NOT_SET, editedIndex) || changed;
  settings.twoFingerSwipeUp = actions[0];
  settings.twoFingerSwipeDown = actions[1];
  settings.twoFingerSwipeLeft = actions[2];
  settings.twoFingerSwipeRight = actions[3];
  return changed;
}

void CrossPointSettings::validateReaderFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.readerFrontButtonBack, settings.readerFrontButtonConfirm,
                             settings.readerFrontButtonLeft, settings.readerFrontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.readerFrontButtonBack = FRONT_HW_BACK;
        settings.readerFrontButtonConfirm = FRONT_HW_CONFIRM;
        settings.readerFrontButtonLeft = FRONT_HW_LEFT;
        settings.readerFrontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::defaultUiScale() { return UI_SCALE_SMALL; }

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

uint8_t CrossPointSettings::sleepScreenStorageToMode(const uint8_t storedValue) {
  if (storedValue < SLEEP_SCREEN_STORAGE_ORDER_COUNT) {
    return SLEEP_SCREEN_STORAGE_ORDER[storedValue];
  }
  return DARK;
}

uint8_t CrossPointSettings::sleepScreenModeToStorage(const uint8_t mode) {
  for (uint8_t storedValue = 0; storedValue < SLEEP_SCREEN_STORAGE_ORDER_COUNT; storedValue++) {
    if (SLEEP_SCREEN_STORAGE_ORDER[storedValue] == mode) {
      return storedValue;
    }
  }
  return 0;
}

uint8_t CrossPointSettings::legacyLineSpacingToPercent(const uint8_t legacyValue, const uint8_t fontFamily,
                                                       const bool sdFontSelected) {
  if (sdFontSelected) {
    switch (legacyValue) {
      case TIGHT:
        return 95;
      case WIDE:
        return 110;
      case NORMAL:
      default:
        return 100;
    }
  }

  switch (fontFamily) {
    case BITTER:
      switch (legacyValue) {
        case TIGHT:
          return 95;
        case WIDE:
          return 130;
        case NORMAL:
        default:
          return 110;
      }
    case LEXENDDECA:
    default:
      switch (legacyValue) {
        case TIGHT:
          return 90;
        case WIDE:
          return 120;
        case NORMAL:
        default:
          return 100;
      }
  }
}

uint8_t CrossPointSettings::clampedLineHeightPercent(const uint8_t value) {
  if (value < MIN_LINE_HEIGHT_PERCENT) return MIN_LINE_HEIGHT_PERCENT;
  if (value > MAX_LINE_HEIGHT_PERCENT) return MAX_LINE_HEIGHT_PERCENT;
  return value;
}

uint8_t CrossPointSettings::readingIdleTimeThresholdUnitsForSeconds(const uint16_t seconds) {
  const uint16_t clampedSeconds =
      std::clamp(seconds, MIN_READING_IDLE_TIME_THRESHOLD_SECONDS, MAX_READING_IDLE_TIME_THRESHOLD_SECONDS);
  return static_cast<uint8_t>((clampedSeconds + READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS - 1) /
                              READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS);
}

uint16_t CrossPointSettings::readingIdleTimeThresholdSecondsForUnits(const uint8_t units) {
  const uint8_t clampedUnits =
      std::clamp(units, MIN_READING_IDLE_TIME_THRESHOLD_UNITS, MAX_READING_IDLE_TIME_THRESHOLD_UNITS);
  return static_cast<uint16_t>(clampedUnits) * READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
}

uint16_t CrossPointSettings::getReadingIdleTimeThresholdSeconds() const {
  return readingIdleTimeThresholdSecondsForUnits(readingIdleTimeThresholdUnits);
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(_mutex);
  for (const auto& info : getBaseSettingsList()) {
    if (!info.key || (!info.valuePtr && !info.value16Ptr && !info.stringOffset)) continue;
    if (info.stringOffset) {
      const char* value = reinterpret_cast<const char*>(this) + info.stringOffset;
      if (info.obfuscated) {
        char key[64];
        snprintf(key, sizeof(key), "%s_obf", info.key);
        doc[key] = obfuscation::obfuscateToBase64(value);
      } else {
        doc[info.key] = value;
      }
    } else if (info.value16Ptr) {
      doc[info.key] = this->*(info.value16Ptr);
    } else {
      uint8_t value = this->*(info.valuePtr);
      if (isSleepScreenSetting(info)) value = sleepScreenModeToStorage(value);
      doc[info.key] = value;
    }
  }

  JsonArray displaySlots = doc["displayStatusBar"].to<JsonArray>();
  for (const auto item : displayStatusBar.slots) displaySlots.add(static_cast<uint8_t>(item));
  doc["displayBatteryStyle"] = static_cast<uint8_t>(displayStatusBar.batteryStyle);

  JsonObject bars = doc["readerStatusBars"].to<JsonObject>();
  bars["version"] = 1;
  writeReaderStatusBarJson(bars["top"].to<JsonObject>(), topReaderStatusBar);
  writeReaderStatusBarJson(bars["bottom"].to<JsonObject>(), bottomReaderStatusBar);
  bars["xtcMode"] = xtcStatusBarMode;
  bars["legacyXtcTopUsesBottom"] = legacyXtcTopUsesBottom != 0;

  // Library-local choices stay out of the resident settings catalog and Web Settings.
  doc["librarySortMethod"] = librarySortMethod;
  doc["librarySortDescending"] = librarySortDescending;
  doc["libraryListExpanded"] = libraryListExpanded;
  doc["recentBooksView"] = recentBooksView;
  doc["libraryShowSeries"] = libraryShowSeries;
  doc["libraryShowGenre"] = libraryShowGenre;
  doc["libraryShowEpub"] = libraryShowEpub;
  doc["libraryShowXtc"] = libraryShowXtc;
  doc["libraryShowTxt"] = libraryShowTxt;
  doc["libraryShowMarkdown"] = libraryShowMarkdown;
  doc["libraryHideFinishedBooks"] = libraryHideFinishedBooks;

  doc["disablePageTurnGestures"] = disablePageTurnGestures;
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  doc["readerFrontButtonsEnabled"] = readerFrontButtonsEnabled;
  doc["readerFrontButtonBack"] = readerFrontButtonBack;
  doc["readerFrontButtonConfirm"] = readerFrontButtonConfirm;
  doc["readerFrontButtonLeft"] = readerFrontButtonLeft;
  doc["readerFrontButtonRight"] = readerFrontButtonRight;
  doc["fontFamily"] = fontFamily;
  if (sdFontFamilyName[0] != '\0') doc["sdFontFamilyName"] = sdFontFamilyName;
  if (dictionarySdFontFamilyName[0] != '\0') doc["dictionaryFont"] = dictionarySdFontFamilyName;
  doc["dictionaryFontSize"] = dictionaryFontPointSize;
  JsonArray quickActionSlotsJson = doc["quickActionSlots"].to<JsonArray>();
  for (const uint8_t action : quickActionSlots) {
    quickActionSlotsJson.add(action);
  }
  doc["quickActionsTrigger"] = quickActionsTrigger;
  doc["filenameFallbackFont"] = filenameFallbackFont;
  doc["language"] = languageCode;
  doc["languageCacheGeneration"] = languageCacheGeneration;
  if (keyboardLayouts != 0) doc["keyboardLayouts"] = keyboardLayouts;
  doc["tiltPageTurnDirectionSchema"] = TILT_DIRECTION_SCHEMA_CURRENT;
  doc["clockDateHasBeenSynced"] = clockDateHasBeenSynced;
  doc["screenInverted"] = screenInverted;
  doc["pluginsEnabled"] = pluginsEnabled;
  doc["pluginHubPromptHidden"] = pluginHubPromptHidden;
}

bool CrossPointSettings::fromJson(JsonVariantConst doc, bool importingCrossPoint) {
  std::lock_guard<std::mutex> lock(_mutex);
  bool needsResave = false;
  auto clamp = [](const uint8_t value, const uint8_t maxValue, const uint8_t fallback) {
    return value < maxValue ? value : fallback;
  };
  const bool migrateLegacyTiltMode = !doc["tiltPageTurn"].isNull() && doc["tiltPageTurnDirection"].isNull();
  const uint8_t legacyTiltMode = doc["tiltPageTurn"] | static_cast<uint8_t>(TILT_OFF);
  const bool migrateTiltDirectionSchema =
      !doc["tiltPageTurnDirection"].isNull() &&
      ((doc["tiltPageTurnDirectionSchema"] | static_cast<uint8_t>(1)) < TILT_DIRECTION_SCHEMA_CURRENT);
  if (doc["statusBarChapterPageCount"].isNull()) applyLegacyStatusBarSettings(*this);
  screenInverted = clamp(doc["screenInverted"] | screenInverted, 2, screenInverted);

  for (const auto& info : getBaseSettingsList()) {
    if (!info.key || (!info.valuePtr && !info.value16Ptr && !info.stringOffset)) continue;
    if (info.stringOffset) {
      char* destination = reinterpret_cast<char*>(this) + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destination[0] = '\0';
        needsResave = true;
        continue;
      }
      const std::string fieldDefault = destination;
      std::string value;
      if (info.obfuscated) {
        char key[64];
        snprintf(key, sizeof(key), "%s_obf", info.key);
        obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
        value = obfuscation::deobfuscateFromBase64(doc[key] | "", info.stringMaxLen - 1, &status);
        if (status == obfuscation::DecodeStatus::LEGACY && !value.empty()) needsResave = true;
        if (status == obfuscation::DecodeStatus::TOO_LONG) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          needsResave = true;
        }
        if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY ||
            status == obfuscation::DecodeStatus::TOO_LONG || value.empty()) {
          const char* legacyValue = doc[info.key] | fieldDefault.c_str();
          if (strlen(legacyValue) >= info.stringMaxLen) {
            LOG_ERR("CPS", "Oversized plaintext value for key '%s'", info.key);
            value = fieldDefault;
            needsResave = true;
          } else {
            value = legacyValue;
            if (value != fieldDefault) needsResave = true;
          }
        }
      } else {
        value = doc[info.key] | fieldDefault;
      }
      strncpy(destination, value.c_str(), info.stringMaxLen - 1);
      destination[info.stringMaxLen - 1] = '\0';
      if (strcmp(info.key, "deviceName") == 0 && strlen(destination) < MIN_DEVICE_NAME_LENGTH) {
        destination[0] = '\0';
        needsResave = true;
      }
      continue;
    }

    if (info.value16Ptr) {
      const uint16_t fieldDefault = this->*(info.value16Ptr);
      uint16_t value = doc[info.key] | fieldDefault;
      if (info.type == SettingType::VALUE) {
        value = std::clamp(value, info.valueRange.min, info.valueRange.max);
      }
      this->*(info.value16Ptr) = value;
      continue;
    }

    // Missing keys in older exports restore the original status font size and
    // bidirectional tilt behavior, even if this session changed them.
    const uint8_t fieldDefault = info.valuePtr == &CrossPointSettings::displayStatusBarTextSize ||
                                         info.valuePtr == &CrossPointSettings::tiltNextPageOnly
                                     ? 0
                                     : this->*(info.valuePtr);
    uint8_t value = doc[info.key] | fieldDefault;
    // CrossPoint stores normal spacing as 100%; CrossInk stores it as level 0.
    // Keep native CrossInk levels intact, including files at the legacy path.
    if (importingCrossPoint && info.valuePtr == &CrossPointSettings::wordSpacing && value == 100) {
      value = 0;
      needsResave = true;
    }
    if (strcmp(info.key, "sdFontSizeRange") == 0 && value == SD_FONT_RANGE_NO_EMOJI_LEGACY) {
      value = SD_FONT_RANGE_ALL;
      needsResave = true;
    }
    if (isSleepScreenSetting(info)) {
      const uint8_t storedDefault = sleepScreenModeToStorage(fieldDefault);
      const uint8_t storedValue = doc[info.key] | storedDefault;
      value = sleepScreenStorageToMode(storedValue);
      if (sleepScreenModeToStorage(value) != storedValue) needsResave = true;
      this->*(info.valuePtr) = value;
      continue;
    }
    if (strcmp(info.key, "fontSize") == 0 && !doc["fontSize"].isNull() && value < MIN_READER_FONT_POINT_SIZE) {
      if (doc["sdFontFamilyName"].is<const char*>() && doc["sdFontFamilyName"].as<const char*>()[0] != '\0') {
        legacySdFontSizeStep = value;
      } else {
        value = getReaderFontPointSize(static_cast<FONT_SIZE>(value));
        needsResave = true;
      }
    } else if (info.type == SettingType::ENUM &&
               !(strcmp(info.key, "fontSize") == 0 && doc["sdFontFamilyName"].is<const char*>() &&
                 doc["sdFontFamilyName"].as<const char*>()[0] != '\0' && value >= MIN_READER_FONT_POINT_SIZE) &&
               !isEnumRawValueAllowed(info, value)) {
      value = defaultEnumRawValue(info, fieldDefault);
      needsResave = true;
    } else if (info.type == SettingType::TOGGLE) {
      value = clamp(value, 2, fieldDefault);
    } else if (info.type == SettingType::VALUE) {
      value = std::clamp(value, static_cast<uint8_t>(info.valueRange.min), static_cast<uint8_t>(info.valueRange.max));
    }
    this->*(info.valuePtr) = value;
  }

  // Retire the old blanket touch gate. Its saved value must not leave hidden restrictions.
  disablePageTurnGestures = clamp(doc["disablePageTurnGestures"] | uint8_t{0}, 2, 0);
  if (!doc["touchReaderControls"].isNull()) needsResave = true;
  const uint8_t legacyFront =
      clamp(doc["longPressButtonBehavior"] | uint8_t{OFF}, LONG_PRESS_BUTTON_BEHAVIOR_COUNT, OFF);
  const auto frontAction = [&](bool left) -> uint8_t {
    switch (legacyFront) {
      case CHAPTER_SKIP:
        return left ? SIDE_PREVIOUS_CHAPTER : SIDE_NEXT_CHAPTER;
      case FONT_SIZE_CHANGE:
        return left ? SIDE_DECREASE_FONT : SIDE_INCREASE_FONT;
      case ORIENTATION_CHANGE:
        return left ? SIDE_ROTATE_CLOCKWISE : SIDE_ROTATE_COUNTERCLOCKWISE;
      case ORIENTATION_FLIP:
        return SIDE_ROTATE_FLIP;
      default:
        return IGNORE;
    }
  };
  if (doc["frontButtonLeftLong"].isNull()) {
    frontButtonLeftLong = frontAction(true);
    needsResave = true;
  }
  if (doc["frontButtonRightLong"].isNull()) {
    frontButtonRightLong = frontAction(false);
    needsResave = true;
  }

  // Old settings assigned one layout and one long-press mode to both side
  // buttons. Populate only missing new keys so partially upgraded settings
  // keep the user's individual choices.
  const uint8_t legacySideLayout =
      clamp(doc["sideButtonLayout"] | static_cast<uint8_t>(PREV_NEXT), SIDE_BUTTON_LAYOUT_COUNT, PREV_NEXT);
  const uint8_t legacySideLong = clamp(doc["sideButtonLongPress"] | static_cast<uint8_t>(SIDE_LONG_CHAPTER_SKIP),
                                       SIDE_LONG_PRESS_COUNT, SIDE_LONG_CHAPTER_SKIP);
  const uint8_t legacyUpShort = legacySideLayout == SIDE_BUTTONS_DISABLED ? IGNORE
                                : legacySideLayout == PREV_NEXT           ? PREVIOUS_PAGE
                                                                          : PAGE_TURN;
  const uint8_t legacyDownShort = legacySideLayout == SIDE_BUTTONS_DISABLED ? IGNORE
                                  : legacySideLayout == NEXT_PREV           ? PREVIOUS_PAGE
                                                                            : PAGE_TURN;
  const auto legacyLongAction = [&](const bool up) -> uint8_t {
    switch (legacySideLong) {
      case SIDE_LONG_CHAPTER_SKIP:
        if (legacySideLayout == SIDE_BUTTONS_DISABLED) return IGNORE;
        return (up ? legacyUpShort : legacyDownShort) == PREVIOUS_PAGE ? SIDE_PREVIOUS_CHAPTER : SIDE_NEXT_CHAPTER;
      case SIDE_LONG_FONT_SIZE:
        return up ? SIDE_INCREASE_FONT : SIDE_DECREASE_FONT;
      case SIDE_LONG_ORIENTATION_CHANGE:
        return up ? SIDE_ROTATE_COUNTERCLOCKWISE : SIDE_ROTATE_CLOCKWISE;
      default:
        return IGNORE;
    }
  };
  if (doc["sideButtonUpShort"].isNull()) {
    sideButtonUpShort = legacyUpShort;
    needsResave = true;
  }
  if (doc["sideButtonDownShort"].isNull()) {
    sideButtonDownShort = legacyDownShort;
    needsResave = true;
  }
  if (doc["sideButtonUpLong"].isNull()) {
    sideButtonUpLong = legacyLongAction(true);
    needsResave = true;
  }
  if (doc["sideButtonDownLong"].isNull()) {
    sideButtonDownLong = legacyLongAction(false);
    needsResave = true;
  }

  const auto readLibraryChoice = [&](const char* key, uint8_t& choice, const int optionCount) {
    const int stored = doc[key] | static_cast<int>(choice);
    if (stored < 0 || stored >= optionCount) {
      needsResave = true;
      return;
    }
    choice = static_cast<uint8_t>(stored);
  };
  readLibraryChoice("librarySortMethod", librarySortMethod, 7);
  readLibraryChoice("librarySortDescending", librarySortDescending, 2);
  readLibraryChoice("libraryListExpanded", libraryListExpanded, 2);
  // A missing or corrupt legacy choice uses List, even if this object loaded another document earlier.
  recentBooksView = RECENT_BOOKS_LIST;
  readLibraryChoice("recentBooksView", recentBooksView, RECENT_BOOKS_VIEW_COUNT);
  readLibraryChoice("libraryShowSeries", libraryShowSeries, 2);
  readLibraryChoice("libraryShowGenre", libraryShowGenre, 2);
  readLibraryChoice("libraryShowEpub", libraryShowEpub, 2);
  readLibraryChoice("libraryShowXtc", libraryShowXtc, 2);
  readLibraryChoice("libraryShowTxt", libraryShowTxt, 2);
  readLibraryChoice("libraryShowMarkdown", libraryShowMarkdown, 2);
  readLibraryChoice("libraryHideFinishedBooks", libraryHideFinishedBooks, 2);

  // Only the generic-file fallback imports CrossPoint's combined touch mode.
  // Explicit CrossInk gesture keys identify a CrossInk document, even at the old path.
  if (importingCrossPoint && doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull()) {
    disableReaderTouchscreen = 0;
    if (doc["touchReaderControls"].is<uint8_t>()) {
      const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
      if (mode <= 3) {
        // CrossPoint: 0=off, 1=tap, 2=swipe, 3=inverted tap.
        pageTurnGesture = mode == 0 ? TAP_AND_SWIPE : mode == 1 ? TAP_ONLY : mode == 2 ? SWIPE_ONLY : INVERTED_TAP;
        previousPageGesture = pageTurnGesture;
      }
    }
    needsResave = true;
  }

  // The old gesture setting controlled both directions. Preserve it on upgrade.
  if (doc["previousPageGesture"].isNull()) {
    previousPageGesture = pageTurnGesture;
    needsResave = true;
  }

  // Older global settings files named the display preference readerDarkMode.
  // Preserve it when moving to the global screenInverted setting.
  if (doc["screenInverted"].isNull() && !doc["readerDarkMode"].isNull()) {
    screenInverted = clamp(doc["readerDarkMode"] | static_cast<uint8_t>(0), 2, 0);
    needsResave = true;
  }
  if (refreshFrequency == REFRESH_NEVER && !Frontlight.present()) {
    refreshFrequency = REFRESH_15;
    needsResave = true;
  }

  const auto normalizeFrontlightScheduleTime = [&needsResave](uint16_t& timeOfDay) {
    if (FrontlightSchedule::isTimeOfDayValid(timeOfDay) || timeOfDay == FrontlightSchedule::kUnsetTimeOfDay) return;
    timeOfDay = FrontlightSchedule::kUnsetTimeOfDay;
    needsResave = true;
  };
  normalizeFrontlightScheduleTime(frontlightScheduleStart);
  normalizeFrontlightScheduleTime(frontlightScheduleEnd);

  if (normalizeTwoFingerSwipeActions(*this)) needsResave = true;
  for (const auto field : {&CrossPointSettings::leftEdgeUp, &CrossPointSettings::leftEdgeDown,
                           &CrossPointSettings::rightEdgeUp, &CrossPointSettings::rightEdgeDown}) {
    if (!isTwoFingerSwipeActionAvailable(this->*field, Frontlight.present(), Frontlight.hasColorTemperature())) {
      this->*field = TWO_FINGER_SWIPE_NOT_SET;
      needsResave = true;
    }
  }

  // The web API shares the base catalog so it can receive raw value 26 even
  // on boards without a Home key. Never retain that reader-only action there.
  if (!gpio.hasHomeKey()) {
    if (shortPwrBtn == TOGGLE_HOME_BUTTON_IN_READER) {
      shortPwrBtn = IGNORE;
      needsResave = true;
    }
    if (longPwrBtn == TOGGLE_HOME_BUTTON_IN_READER) {
      longPwrBtn = SLEEP;
      needsResave = true;
    }
    if (homeButtonTapAction == QUICK_ACTIONS) {
      homeButtonTapAction = HOME_BUTTON_BACK_HOME;
      needsResave = true;
    }
    if (homeButtonLongPressAction == QUICK_ACTIONS) {
      homeButtonLongPressAction = HOME_BUTTON_READER_MENU;
      needsResave = true;
    }
    if (homeButtonDoubleTapAction == QUICK_ACTIONS) {
      homeButtonDoubleTapAction = HOME_BUTTON_TOGGLE_FRONTLIGHT;
      needsResave = true;
    }
  }

  if (doc["fileBrowserDisplay"].isNull()) {
    if (!doc["fileBrowserPreview"].isNull()) {
      fileBrowserDisplay = clamp(doc["fileBrowserPreview"] | static_cast<uint8_t>(FILE_BROWSER_DISPLAY_1_LINE),
                                 FILE_BROWSER_DISPLAY_COUNT, FILE_BROWSER_DISPLAY_1_LINE);
      needsResave = true;
    } else if (uiTheme == MINIMAL) {
      fileBrowserDisplay = FILE_BROWSER_DISPLAY_2_LINES;
      needsResave = true;
    }
  }

  if (migrateLegacyTiltMode) {
    if (legacyTiltMode == 1 || legacyTiltMode == 2) {
      tiltPageTurn = TILT_ON;
      tiltPageTurnDirection = legacyTiltMode == 2 ? TILT_LEFT_RIGHT : TILT_LEFT_RIGHT_INVERTED;
      needsResave = true;
    } else {
      tiltPageTurn = TILT_OFF;
    }
  } else if (migrateTiltDirectionSchema) {
    tiltPageTurnDirection = migrateTiltDirectionValue(tiltPageTurnDirection);
    needsResave = true;
  }
  if (!doc["tiltPageTurnDirection"].isNull() && doc["tiltPageTurnDirectionSchema"].isNull()) needsResave = true;

  if (doc["hideClock"].isNull() && !doc["statusBarClock"].isNull()) {
    constexpr uint8_t LEGACY_SHOW_CLOCK_NEVER = 0;
    const uint8_t legacyShowClock =
        clamp(doc["statusBarClock"] | LEGACY_SHOW_CLOCK_NEVER, HIDE_CLOCK_MODE_COUNT, LEGACY_SHOW_CLOCK_NEVER);
    hideClock = legacyShowClock == LEGACY_SHOW_CLOCK_NEVER ? HIDE_CLOCK_ALWAYS : HIDE_CLOCK_NEVER;
    needsResave = true;
  }
  // Saved layouts must survive a temporarily unavailable RTC; rendering filters unavailable items.
  const JsonArrayConst displaySlots = doc["displayStatusBar"].as<JsonArrayConst>();
  if (displaySlots.size() == displayStatusBar.slots.size()) {
    for (unsigned i = 0; i < displayStatusBar.slots.size(); ++i) {
      const int item = displaySlots[i].as<int>();
      if (displaySlots[i].is<int>() && validDisplayStatusBarItemValue(item, true)) {
        displayStatusBar.slots[i] = static_cast<ReaderStatusBarItem>(item);
      } else {
        displayStatusBar.slots[i] = ReaderStatusBarItem::Empty;
        needsResave = true;
      }
    }
  } else {
    displayStatusBar = DisplayStatusBarConfig{};
    const bool legacyClock = doc["showClockOutsideReader"].isNull() ? hideClock != HIDE_CLOCK_ALWAYS
                                                                    : (doc["showClockOutsideReader"].as<int>() != 0);
    if (legacyClock) displayStatusBar.slots[1] = ReaderStatusBarItem::Clock;
    needsResave = true;
  }
  const JsonVariantConst bars = doc["readerStatusBars"];
  if (bars["version"] != 1) {
    topReaderStatusBar = ReaderStatusBarConfig{};
    bottomReaderStatusBar = migrateBottomStatusBar(
        {statusBarChapterPageCount != 0, stablePageNumbers != 0, statusBarBookProgressPercentage != 0, statusBarTitle,
         statusBarTimeLeft, statusBarBattery != 0, statusBarBookPercentageFormat, statusBarProgressBar,
         statusBarProgressBarThickness});
    if (hideClock == HIDE_CLOCK_NEVER) {
      topReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Clock;
    }
    legacyXtcTopUsesBottom = xtcStatusBarMode == XTC_STATUS_BAR_TOP;
    needsResave = true;
  } else {
    needsResave |= repairReaderStatusBarJson(bars["top"], topReaderStatusBar, true, BOOK_PERCENTAGE_FORMAT_COUNT,
                                             STATUS_BAR_PROGRESS_BAR_COUNT, STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT);
    needsResave |= repairReaderStatusBarJson(bars["bottom"], bottomReaderStatusBar, true, BOOK_PERCENTAGE_FORMAT_COUNT,
                                             STATUS_BAR_PROGRESS_BAR_COUNT, STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT);
    const int xtcMode = bars["xtcMode"].as<int>();
    if (bars["xtcMode"].is<int>() && xtcMode >= 0 && xtcMode < XTC_STATUS_BAR_MODE_COUNT) {
      xtcStatusBarMode = xtcMode;
    } else {
      needsResave = true;
    }
    legacyXtcTopUsesBottom = bars["legacyXtcTopUsesBottom"].as<bool>() ? 1 : 0;
  }
  const uint8_t legacyHideBattery =
      clamp(doc["hideBatteryPercentage"] | static_cast<uint8_t>(HIDE_NEVER), HIDE_BATTERY_PERCENTAGE_COUNT, HIDE_NEVER);
  const auto migrateReaderBatteryStyle = [&](const char* key, ReaderStatusBarConfig& bar) {
    if (bars["version"] == 1 && !bars[key]["battery"].isUnbound()) return;
    bar.batteryStyle = legacyBatteryStyle(legacyHideBattery, false);
    needsResave = true;
  };
  migrateReaderBatteryStyle("top", topReaderStatusBar);
  migrateReaderBatteryStyle("bottom", bottomReaderStatusBar);
  if (doc["displayBatteryStyle"].isUnbound()) {
    displayStatusBar.batteryStyle = legacyBatteryStyle(legacyHideBattery, true);
    needsResave = true;
  } else if (!readReaderStatusBarBatteryStyle(doc["displayBatteryStyle"], displayStatusBar.batteryStyle)) {
    displayStatusBar.batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;
    needsResave = true;
  }
  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | static_cast<uint8_t>(SLEEP_10_MIN), SLEEP_TIMEOUT_COUNT, SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }

  frontButtonBack =
      clamp(doc["frontButtonBack"] | static_cast<uint8_t>(FRONT_HW_BACK), FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm = clamp(doc["frontButtonConfirm"] | static_cast<uint8_t>(FRONT_HW_CONFIRM),
                             FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft =
      clamp(doc["frontButtonLeft"] | static_cast<uint8_t>(FRONT_HW_LEFT), FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight = clamp(doc["frontButtonRight"] | static_cast<uint8_t>(FRONT_HW_RIGHT), FRONT_BUTTON_HARDWARE_COUNT,
                           FRONT_HW_RIGHT);
  validateFrontButtonMapping(*this);
  readerFrontButtonsEnabled = clamp(doc["readerFrontButtonsEnabled"] | static_cast<uint8_t>(0), 2, 0);
  readerFrontButtonBack = clamp(doc["readerFrontButtonBack"] | static_cast<uint8_t>(FRONT_HW_BACK),
                                FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  readerFrontButtonConfirm = clamp(doc["readerFrontButtonConfirm"] | static_cast<uint8_t>(FRONT_HW_CONFIRM),
                                   FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  readerFrontButtonLeft = clamp(doc["readerFrontButtonLeft"] | static_cast<uint8_t>(FRONT_HW_LEFT),
                                FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  readerFrontButtonRight = clamp(doc["readerFrontButtonRight"] | static_cast<uint8_t>(FRONT_HW_RIGHT),
                                 FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateReaderFrontButtonMapping(*this);

  const uint8_t storedFontFamily = doc["fontFamily"] | static_cast<uint8_t>(0);
  fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, 0);
  const char* sdFamily = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sdFamily, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  const char* dictionaryFamily = doc["dictionaryFont"] | "";
  strncpy(dictionarySdFontFamilyName, dictionaryFamily, sizeof(dictionarySdFontFamilyName) - 1);
  dictionarySdFontFamilyName[sizeof(dictionarySdFontFamilyName) - 1] = '\0';
  dictionaryFontPointSize = doc["dictionaryFontSize"] | static_cast<uint8_t>(0);
  const JsonArrayConst quickActionSlotsJson = doc["quickActionSlots"].as<JsonArrayConst>();
  if (!quickActionSlotsJson.isNull()) {
    for (size_t i = 0; i < std::size(quickActionSlots); ++i) {
      const uint8_t action = quickActionSlotsJson[i] | static_cast<uint8_t>(IGNORE);
      if (isValidQuickActionSlot(action)) {
        quickActionSlots[i] = action;
      } else {
        quickActionSlots[i] = IGNORE;
        needsResave = true;
      }
    }
  }
  const uint8_t persistedQuickActionsTrigger =
      doc["quickActionsTrigger"] | static_cast<uint8_t>(QuickActions::Trigger::None);
  const bool unavailableHomeTrigger =
      !gpio.hasHomeKey() && persistedQuickActionsTrigger >= static_cast<uint8_t>(QuickActions::Trigger::TapHome) &&
      persistedQuickActionsTrigger <= static_cast<uint8_t>(QuickActions::Trigger::DoubleTapHome);
  if (persistedQuickActionsTrigger <= static_cast<uint8_t>(QuickActions::Trigger::FrontRightLong) &&
      !unavailableHomeTrigger) {
    quickActionsTrigger = persistedQuickActionsTrigger;
  } else {
    quickActionsTrigger = static_cast<uint8_t>(QuickActions::Trigger::None);
    needsResave = true;
  }
  if (storedFontFamily >= BUILTIN_FONT_COUNT) needsResave = true;
  if (doc["lineHeightPercent"].isNull() && !doc["lineSpacing"].isNull()) {
    const uint8_t legacySpacing =
        clamp(doc["lineSpacing"] | static_cast<uint8_t>(NORMAL), LINE_COMPRESSION_COUNT, static_cast<uint8_t>(NORMAL));
    lineHeightPercent = legacyLineSpacingToPercent(legacySpacing, fontFamily, sdFontFamilyName[0] != '\0');
    needsResave = true;
  }
  if (doc["screenMarginVertical"].isNull() || doc["screenMarginHorizontal"].isNull()) {
    const uint8_t legacyScreenMargin =
        std::clamp(doc["screenMargin"] | static_cast<uint8_t>(MIN_SCREEN_MARGIN), MIN_SCREEN_MARGIN, MAX_SCREEN_MARGIN);
    if (doc["screenMarginVertical"].isNull()) screenMarginVertical = legacyScreenMargin;
    if (doc["screenMarginHorizontal"].isNull()) screenMarginHorizontal = legacyScreenMargin;
    needsResave = true;
  }
  if (doc["language"].is<const char*>()) {
    const char* code = doc["language"].as<const char*>();
    if (language_cache::validCode(code)) {
      std::strcpy(languageCode, code);
      for (char* p = languageCode; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
    }
  } else if (doc["language"].is<uint8_t>()) {
    const auto legacy = doc["language"].as<uint8_t>();
    if (legacy < getLanguageCount()) std::strcpy(languageCode, LANGUAGE_CODES[legacy]);
  }
  languageCacheGeneration = doc["languageCacheGeneration"] | uint64_t{0};
  const char* filenameFont = doc["filenameFallbackFont"] | "";
  if (std::strlen(filenameFont) < sizeof(filenameFallbackFont) && !std::strchr(filenameFont, '/') &&
      !std::strchr(filenameFont, '\\') && std::strcmp(filenameFont, ".") != 0 && std::strcmp(filenameFont, "..") != 0) {
    std::strcpy(filenameFallbackFont, filenameFont);
  }
  if (doc["keyboardLayouts"].is<uint16_t>()) {
    keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();
  }
  clockDateHasBeenSynced = clamp(doc["clockDateHasBeenSynced"] | static_cast<uint8_t>(0), 2, 0);

  if (doc["pluginsEnabled"].isNull()) {
    pluginsEnabled = PluginLocations::anyPluginInstalledOnDisk() ? 1 : 0;
    needsResave = true;
  } else {
    pluginsEnabled = clamp(doc["pluginsEnabled"] | uint8_t{0}, 2, 0);
  }
  pluginHubPromptHidden = clamp(doc["pluginHubPromptHidden"] | uint8_t{0}, 2, 0);
  if (needsResave) requestResave();
  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

bool CrossPointSettings::saveToFile() const {
  std::lock_guard<std::mutex> lock(storeMutex);
  JsonDocument doc;
  toJson(doc);
  return PersistableStoreBase::writeDocToFileAtomic(SETTINGS_FILE_JSON, doc);
}

bool CrossPointSettings::loadFromFile() {
  if (!PersistableStoreBase::recoverAtomicFile(SETTINGS_FILE_JSON)) return false;
  enum class JsonLoadStatus : uint8_t { MissingOrEmpty, Loaded, Failed };

  auto loadJsonSettings = [this](const char* path, bool migrateToCurrentPath) -> JsonLoadStatus {
    JsonDocument doc;
    if (PersistableStoreBase::readDocFromFile(path, doc)) {
      bool result = false;
      bool resave = false;
      {
        std::lock_guard<std::mutex> storeLock(storeMutex);
        resaveRequested = false;
        result = fromJson(doc.as<JsonVariantConst>(), migrateToCurrentPath);
        resave = resaveRequested;
        resaveRequested = false;
      }
      if (result) {
        std::lock_guard<std::mutex> settingsLock(_mutex);
        if (restoreLegacyRtcDateSyncState(*this)) resave = true;
      }
      if (result && (resave || migrateToCurrentPath)) {
        if (saveToFile()) {
          LOG_DBG("CPS", migrateToCurrentPath ? "Migrated legacy settings.json to crossink-settings.json"
                                              : "Resaved settings to update format");
        } else {
          LOG_ERR("CPS", migrateToCurrentPath ? "Failed to save migrated settings to crossink-settings.json"
                                              : "Failed to resave settings after format update");
        }
      }
      migrateLanguageBinaryFile();
      return result ? JsonLoadStatus::Loaded : JsonLoadStatus::Failed;
    }
    return JsonLoadStatus::MissingOrEmpty;
  };

  // Prefer CrossInk's namespaced settings file. Use the old generic file only
  // as a migration fallback so other firmware can keep its own settings.json.
  const bool hasCrossInkSettings = Storage.exists(SETTINGS_FILE_JSON) || Storage.exists(SETTINGS_FILE_JSON_BAK);
  JsonLoadStatus jsonStatus = loadJsonSettings(SETTINGS_FILE_JSON, false);
  // A CrossInk-specific settings file takes precedence even when it is
  // damaged. Falling through would import another firmware's settings.json
  // and replace the user's CrossInk preferences.
  if (hasCrossInkSettings || jsonStatus != JsonLoadStatus::MissingOrEmpty) {
    return jsonStatus == JsonLoadStatus::Loaded;
  }

  jsonStatus = loadJsonSettings(LEGACY_SETTINGS_FILE_JSON, true);
  if (jsonStatus != JsonLoadStatus::MissingOrEmpty) return jsonStatus == JsonLoadStatus::Loaded;

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      applyLegacyStatusBarSettings(*this);
      bottomReaderStatusBar = migrateBottomStatusBar(
          {statusBarChapterPageCount != 0, stablePageNumbers != 0, statusBarBookProgressPercentage != 0, statusBarTitle,
           statusBarTimeLeft, statusBarBattery != 0, statusBarBookPercentageFormat, statusBarProgressBar,
           statusBarProgressBarThickness});
      topReaderStatusBar.batteryStyle = legacyBatteryStyle(hideBatteryPercentage, false);
      bottomReaderStatusBar.batteryStyle = legacyBatteryStyle(hideBatteryPercentage, false);
      displayStatusBar.batteryStyle = legacyBatteryStyle(hideBatteryPercentage, true);
      migrateLanguageBinaryFile();
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to crossink-settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  // No settings files at all -- check for standalone language.bin
  return migrateLanguageBinaryFile();
}

bool CrossPointSettings::migrateLanguageBinaryFile() {
  // V1_LANGUAGES / V1_LANGUAGE_COUNT are emitted by gen_i18n.py with the
  // frozen enum order from 2f969a9.
  if (!Storage.exists(LANG_FILE_BIN)) return false;

  FsFile f;
  if (Storage.openFileForRead("CPS", LANG_FILE_BIN, f)) {
    uint8_t version;
    serialization::readPod(f, version);
    if (version == 1) {
      uint8_t oldIndex;
      serialization::readPod(f, oldIndex);
      if (oldIndex < V1_LANGUAGE_COUNT) {
        std::strcpy(languageCode, LANGUAGE_CODES[static_cast<uint8_t>(V1_LANGUAGES[oldIndex])]);
        languageCacheGeneration = 0;
      }
    }
  }
  f.close();
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into crossink-settings.json");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    uint8_t storedSleepScreen = sleepScreenModeToStorage(sleepScreen);
    readAndValidate(inputFile, storedSleepScreen, SLEEP_SCREEN_STORAGE_ORDER_COUNT);
    sleepScreen = sleepScreenStorageToMode(storedSleepScreen);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      uint8_t legacyFontFamily;
      serialization::readPod(inputFile, legacyFontFamily);
      if (legacyFontFamily < BUILTIN_FONT_COUNT) {
        fontFamily = legacyFontFamily;
      }
    }
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacyFontSize = MEDIUM;
    readAndValidate(inputFile, legacyFontSize, getActiveReaderFontSizeCount());
    readerFontPointSize = getReaderFontPointSize(static_cast<FONT_SIZE>(legacyFontSize));
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacySleepTimeout = SLEEP_10_MIN;
    readAndValidate(inputFile, legacySleepTimeout, SLEEP_TIMEOUT_COUNT);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacySleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacyScreenMargin = MIN_SCREEN_MARGIN;
    serialization::readPod(inputFile, legacyScreenMargin);
    screenMarginVertical = std::clamp(legacyScreenMargin, MIN_SCREEN_MARGIN, MAX_SCREEN_MARGIN);
    screenMarginHorizontal = screenMarginVertical;
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      // Older builds wrote uiTheme via raw readPod, so any byte (including
      // values that were briefly assigned to themes that are not currently
      // exposed) may be on disk. Map anything outside the active theme count
      // to LYRA so the migration is deterministic instead of leaning on
      // readAndValidate's no-op-on-invalid behaviour.
      uint8_t rawTheme = LYRA;
      serialization::readPod(inputFile, rawTheme);
      uiTheme = (rawTheme < UI_THEME_COUNT) ? rawTheme : static_cast<uint8_t>(LYRA);
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  switch (sideButtonLayout) {
    case NEXT_PREV:
      sideButtonUpShort = PAGE_TURN;
      sideButtonDownShort = PREVIOUS_PAGE;
      sideButtonUpLong = SIDE_NEXT_CHAPTER;
      sideButtonDownLong = SIDE_PREVIOUS_CHAPTER;
      break;
    case SIDE_BUTTONS_DISABLED:
      sideButtonUpShort = IGNORE;
      sideButtonDownShort = IGNORE;
      sideButtonUpLong = IGNORE;
      sideButtonDownLong = IGNORE;
      break;
    case NEXT_NEXT:
      sideButtonUpShort = PAGE_TURN;
      sideButtonDownShort = PAGE_TURN;
      sideButtonUpLong = SIDE_NEXT_CHAPTER;
      sideButtonDownLong = SIDE_NEXT_CHAPTER;
      break;
    default:
      break;
  }

  switch (longPressButtonBehavior) {
    case CHAPTER_SKIP:
      frontButtonLeftLong = SIDE_PREVIOUS_CHAPTER;
      frontButtonRightLong = SIDE_NEXT_CHAPTER;
      break;
    case FONT_SIZE_CHANGE:
      frontButtonLeftLong = SIDE_DECREASE_FONT;
      frontButtonRightLong = SIDE_INCREASE_FONT;
      break;
    case ORIENTATION_CHANGE:
      frontButtonLeftLong = SIDE_ROTATE_CLOCKWISE;
      frontButtonRightLong = SIDE_ROTATE_COUNTERCLOCKWISE;
      break;
    case ORIENTATION_FLIP:
      frontButtonLeftLong = frontButtonRightLong = SIDE_ROTATE_FLIP;
      break;
    default:
      frontButtonLeftLong = frontButtonRightLong = IGNORE;
      break;
  }

  lineHeightPercent = legacyLineSpacingToPercent(lineSpacing, fontFamily, sdFontFamilyName[0] != '\0');

  return true;
}

ReaderStatusBarConfig CrossPointSettings::readerStatusBar(const ReaderStatusBarPosition position) const {
  std::lock_guard<std::mutex> lock(_mutex);
  return position == ReaderStatusBarPosition::Top ? topReaderStatusBar : bottomReaderStatusBar;
}

bool CrossPointSettings::parseReaderStatusBars(JsonVariantConst json, ReaderStatusBarsPayload& config) {
  return readReaderStatusBarsPayload(json, config, halClock.isAvailable(), BOOK_PERCENTAGE_FORMAT_COUNT,
                                     STATUS_BAR_PROGRESS_BAR_COUNT, STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT,
                                     XTC_STATUS_BAR_MODE_COUNT);
}

void CrossPointSettings::setReaderStatusBar(const ReaderStatusBarPosition position,
                                            const ReaderStatusBarConfig& config) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (position == ReaderStatusBarPosition::Top) {
    if (topReaderStatusBar.slots != config.slots || topReaderStatusBar.percentageFormat != config.percentageFormat ||
        topReaderStatusBar.progressBar != config.progressBar ||
        topReaderStatusBar.progressBarThickness != config.progressBarThickness ||
        topReaderStatusBar.batteryStyle != config.batteryStyle || topReaderStatusBar.centered != config.centered ||
        topReaderStatusBar.separator != config.separator) {
      legacyXtcTopUsesBottom = 0;
    }
    topReaderStatusBar = config;
  } else {
    bottomReaderStatusBar = config;
  }
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth, const uint16_t viewportHeight,
                                                      const EpubRenderMode renderMode) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.forceParagraphIndents = forceParagraphIndents != 0;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  spec.guideReadingEnabled = guideReadingEnabled != 0;
  spec.wordSpacing = wordSpacing;
  spec.characterSpacing = characterSpacingLevel(characterSpacing);
  spec.renderMode = renderMode;
#if CROSSINK_SCALABLE_FONTS
  if (spec.embeddedStyle && renderMode != EpubRenderMode::Light &&
      publisherFonts <= static_cast<uint8_t>(PublisherFonts::All))
    spec.publisherFonts = static_cast<PublisherFonts>(publisherFonts);
#endif
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  return static_cast<float>(clampedLineHeightPercent(lineHeightPercent)) / 100.0f;
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

#ifdef SIMULATOR
bool CrossPointSettings::verifySleepTimeoutMigrationContract() {
  CrossPointSettings& settings = getInstance();
  const uint8_t originalMinutes = settings.sleepTimeoutMinutes;

  settings.sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(SLEEP_5_MIN);
  const bool migratedValueDrivesTimeout = settings.getSleepTimeoutMs() == 5UL * 60UL * 1000UL;

  settings.sleepTimeoutMinutes = 12;
  const bool runtimeUsesMinutesOnly = settings.getSleepTimeoutMs() == 12UL * 60UL * 1000UL;

  settings.sleepTimeoutMinutes = originalMinutes;
  return migratedValueDrivesTimeout && runtimeUsesMinutesOnly;
}

bool CrossPointSettings::verifySleepScreenMigrationContract() {
  constexpr uint8_t legacyModeCountBeforeMinimal = 8;
  constexpr uint8_t minimalSleepStorageValue = 8;
  constexpr uint8_t quickResumeStorageValue = 9;
  constexpr uint8_t minimalStatsStorageValue = 10;
  constexpr uint8_t dashboardSleepStorageValue = 11;
  for (uint8_t storedValue = 0; storedValue < legacyModeCountBeforeMinimal; storedValue++) {
    if (sleepScreenStorageToMode(storedValue) != storedValue) {
      return false;
    }
  }

  return sleepScreenStorageToMode(minimalSleepStorageValue) == MINIMAL_SLEEP &&
         sleepScreenModeToStorage(MINIMAL_SLEEP) == minimalSleepStorageValue &&
         sleepScreenStorageToMode(quickResumeStorageValue) == QUICK_RESUME &&
         sleepScreenModeToStorage(QUICK_RESUME) == quickResumeStorageValue &&
         sleepScreenStorageToMode(minimalStatsStorageValue) == MINIMAL_STATS_SLEEP &&
         sleepScreenModeToStorage(MINIMAL_STATS_SLEEP) == minimalStatsStorageValue &&
         sleepScreenStorageToMode(dashboardSleepStorageValue) == DASHBOARD_SLEEP &&
         sleepScreenModeToStorage(DASHBOARD_SLEEP) == dashboardSleepStorageValue &&
         sleepScreenStorageToMode(12) == COVER_OVERLAY && sleepScreenModeToStorage(COVER_OVERLAY) == 12 &&
         sleepScreenStorageToMode(UINT8_MAX) == DARK;
}
#endif

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_NEVER:
      // Never is available only on frontlit boards. Persisted settings can be
      // shared between devices, so retain a safe cadence everywhere else.
      return Frontlight.present() ? std::numeric_limits<int>::max() : 15;
  }
}

uint8_t CrossPointSettings::getActiveReaderFontSizeCount() {
  return static_cast<uint8_t>(std::count_if(std::begin(READER_FONT_SIZE_STORAGE_ORDER),
                                            std::end(READER_FONT_SIZE_STORAGE_ORDER),
                                            [](const FONT_SIZE size) { return isReaderFontSizeAvailable(size); }));
}

uint8_t CrossPointSettings::getStoredReaderFontSize(const FONT_SIZE size) {
  uint8_t stored = 0;
  for (const FONT_SIZE activeSize : READER_FONT_SIZE_STORAGE_ORDER) {
    if (!isReaderFontSizeAvailable(activeSize)) continue;
    if (size == activeSize) return stored;
    stored++;
  }
  return INVALID_READER_FONT_SIZE;
}

uint8_t CrossPointSettings::getReaderFontPointSize(const FONT_SIZE size) {
  switch (size) {
    case TINY:
      return 10;
    case SMALL:
      return 12;
    case MEDIUM:
    default:
      return 14;
    case LARGE:
      return 16;
  }
}

uint8_t CrossPointSettings::getSdFontRangePointSize(uint8_t range, uint8_t step) {
  range = normalizedSdFontRange(range);
  const uint8_t stepCount = SD_FONT_RANGE_STEP_COUNTS[range];
  if (step >= stepCount) step = stepCount - 1;
  return SD_FONT_RANGE_POINT_SIZES[range][step];
}

bool CrossPointSettings::isSdFontPointSizeAllowedForRange(const uint8_t pointSize, const uint8_t range) {
  const uint8_t normalizedRange = normalizedSdFontRange(range);
  const uint8_t stepCount = SD_FONT_RANGE_STEP_COUNTS[normalizedRange];
  for (uint8_t i = 0; i < stepCount; i++) {
    if (SD_FONT_RANGE_POINT_SIZES[normalizedRange][i] == pointSize) return true;
  }
  return false;
}

CrossPointSettings::FONT_SIZE CrossPointSettings::getEffectiveReaderFontSize() const {
  FONT_SIZE best = firstAvailableReaderFontSize();
  uint8_t bestDiff = UINT8_MAX;
  for (const FONT_SIZE size : READER_FONT_SIZE_STORAGE_ORDER) {
    if (!isReaderFontSizeAvailable(size)) continue;
    const uint8_t pointSize = getReaderFontPointSize(size);
    const uint8_t diff =
        pointSize > readerFontPointSize ? pointSize - readerFontPointSize : readerFontPointSize - pointSize;
    if (diff < bestDiff || (diff == bestDiff && pointSize < getReaderFontPointSize(best))) {
      best = size;
      bestDiff = diff;
    }
  }
  return best;
}

uint8_t CrossPointSettings::getSdFontTargetPointSize() const { return readerFontPointSize; }

bool CrossPointSettings::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  return changeReaderFontSizeStep(BUILTIN_READER_FONT_SIZES, std::size(BUILTIN_READER_FONT_SIZES), readerFontPointSize,
                                  larger, mode);
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, readerFontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  return getBuiltInReaderFontId();
}

int CrossPointSettings::getBuiltInReaderFontId() const {
#if CROSSINK_SCALABLE_FONTS
  return scalableBuiltinReaderFontId(fontFamily == BITTER ? 1 : 0, closestBuiltinReaderPointSize(readerFontPointSize));
#else
  const FONT_SIZE effectiveSize = getEffectiveReaderFontSize();

  switch (fontFamily) {
    case LEXENDDECA:
    default:
      switch (effectiveSize) {
        case TINY:
          return LEXENDDECA_10_FONT_ID;
        case SMALL:
          return LEXENDDECA_12_FONT_ID;
        case MEDIUM:
        default:
          return LEXENDDECA_14_FONT_ID;
        case LARGE:
          return LEXENDDECA_16_FONT_ID;
      }
      return getFallbackReaderFontIdForFamily(LEXENDDECA);
    case BITTER:
      switch (effectiveSize) {
        case TINY:
          return BITTER_10_FONT_ID;
        case SMALL:
          return BITTER_12_FONT_ID;
        case MEDIUM:
        default:
          return BITTER_14_FONT_ID;
        case LARGE:
          return BITTER_16_FONT_ID;
      }
      return getFallbackReaderFontIdForFamily(BITTER);
  }
  return getFallbackReaderFontIdForFamily(static_cast<FONT_FAMILY>(fontFamily));
#endif
}

void CrossPointSettings::formatCharacterSpacing(const int value, char* buffer, const size_t length) {
  const int level = characterSpacingLevel(static_cast<uint8_t>(std::clamp(value, 0, 10)));
  snprintf(buffer, length, level > 0 ? "+%d" : "%d", level);
}

void CrossPointSettings::formatWordSpacingSlider(const int value, char* buffer, const size_t length) {
  const int level = WordSpacing::level(WordSpacing::fromSlider(value));
  snprintf(buffer, length, "%d", level);
}
