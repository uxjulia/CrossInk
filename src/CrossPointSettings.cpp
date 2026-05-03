#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "I18nKeys.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";
constexpr uint8_t INVALID_READER_FONT_SIZE = 0xFF;
constexpr CrossPointSettings::FONT_SIZE READER_FONT_SIZE_STORAGE_ORDER[] = {
    CrossPointSettings::TINY,     CrossPointSettings::SMALL,       CrossPointSettings::MEDIUM,
    CrossPointSettings::LARGE,    CrossPointSettings::EXTRA_LARGE, CrossPointSettings::TEENSY,
    CrossPointSettings::HUGE_SIZE};
constexpr CrossPointSettings::FONT_SIZE READER_FONT_SIZE_CYCLE_ORDER[] = {
    CrossPointSettings::TEENSY,   CrossPointSettings::TINY,  CrossPointSettings::SMALL,
    CrossPointSettings::MEDIUM,   CrossPointSettings::LARGE, CrossPointSettings::EXTRA_LARGE,
    CrossPointSettings::HUGE_SIZE};

bool isReaderFontSizeAvailable(const CrossPointSettings::FONT_SIZE size) {
  switch (size) {
    case CrossPointSettings::TEENSY:
#ifdef OMIT_TEENSY_FONT
      return false;
#else
      return true;
#endif
    case CrossPointSettings::TINY:
#ifdef OMIT_TINY_FONT
      return false;
#else
      return true;
#endif
    case CrossPointSettings::SMALL:
#ifdef OMIT_SMALL_FONT
      return false;
#else
      return true;
#endif
    case CrossPointSettings::EXTRA_LARGE:
#ifdef OMIT_XLARGE_FONT
      return false;
#else
      return true;
#endif
    case CrossPointSettings::HUGE_SIZE:
#ifdef OMIT_HUGE_FONT
      return false;
#else
      return true;
#endif
    case CrossPointSettings::MEDIUM:
    case CrossPointSettings::LARGE:
    default:
      return true;
  }
}

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

}  // namespace

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

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  // Try JSON first
  if (Storage.exists(SETTINGS_FILE_JSON)) {
    String json = Storage.readFile(SETTINGS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      if (result && resave) {
        if (saveToFile()) {
          LOG_DBG("CPS", "Resaved settings to update format");
        } else {
          LOG_ERR("CPS", "Failed to resave settings after format update");
        }
      }
      migrateLanguageBinaryFile();
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      migrateLanguageBinaryFile();
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
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
        language = static_cast<uint8_t>(V1_LANGUAGES[oldIndex]);
      }
    }
  }
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into settings.json");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

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
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
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
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, getActiveReaderFontSizeCount());
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
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

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case LEXENDDECA:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.2f;
      }
    case CHAREINK:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.1f;
        case WIDE:
          return 1.3f;
      }
    case BITTER:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.1f;
        case WIDE:
          return 1.3f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

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

CrossPointSettings::FONT_SIZE CrossPointSettings::getEffectiveReaderFontSize() const {
  uint8_t stored = 0;
  for (const FONT_SIZE size : READER_FONT_SIZE_STORAGE_ORDER) {
    if (!isReaderFontSizeAvailable(size)) continue;
    if (fontSize == stored) return size;
    stored++;
  }
  return MEDIUM;
}

bool CrossPointSettings::changeReaderFontSize(const bool larger) {
  const FONT_SIZE currentSize = getEffectiveReaderFontSize();
  int currentIndex = 0;
  constexpr size_t sizeCount = sizeof(READER_FONT_SIZE_CYCLE_ORDER) / sizeof(READER_FONT_SIZE_CYCLE_ORDER[0]);
  for (size_t i = 0; i < sizeCount; i++) {
    if (READER_FONT_SIZE_CYCLE_ORDER[i] == currentSize) {
      currentIndex = static_cast<int>(i);
      break;
    }
  }

  for (size_t step = 1; step < sizeCount; step++) {
    const int direction = larger ? 1 : -1;
    const size_t nextIndex =
        (currentIndex + direction * static_cast<int>(step) + static_cast<int>(sizeCount)) % sizeCount;
    const uint8_t stored = getStoredReaderFontSize(READER_FONT_SIZE_CYCLE_ORDER[nextIndex]);
    if (stored != INVALID_READER_FONT_SIZE) {
      fontSize = stored;
      return true;
    }
  }
  return false;
}

int CrossPointSettings::getReaderFontId() const {
  const FONT_SIZE effectiveSize = getEffectiveReaderFontSize();
  switch (fontFamily) {
    case LEXENDDECA:
    default:
      switch (effectiveSize) {
#ifndef OMIT_TEENSY_FONT
        case TEENSY:
          return LEXENDDECA_8_FONT_ID;
#endif
#ifndef OMIT_TINY_FONT
        case TINY:
          return LEXENDDECA_10_FONT_ID;
#endif
#ifndef OMIT_SMALL_FONT
        case SMALL:
          return LEXENDDECA_12_FONT_ID;
#endif
        case MEDIUM:
        default:
          return LEXENDDECA_14_FONT_ID;
        case LARGE:
          return LEXENDDECA_16_FONT_ID;
#ifndef OMIT_XLARGE_FONT
        case EXTRA_LARGE:
          return LEXENDDECA_18_FONT_ID;
#endif
#ifndef OMIT_HUGE_FONT
        case HUGE_SIZE:
          return LEXENDDECA_20_FONT_ID;
#endif
      }
    case CHAREINK:
      switch (effectiveSize) {
#ifndef OMIT_TEENSY_FONT
        case TEENSY:
          return CHAREINK_8_FONT_ID;
#endif
#ifndef OMIT_TINY_FONT
        case TINY:
          return CHAREINK_10_FONT_ID;
#endif
#ifndef OMIT_SMALL_FONT
        case SMALL:
          return CHAREINK_12_FONT_ID;
#endif
        case MEDIUM:
        default:
          return CHAREINK_14_FONT_ID;
        case LARGE:
          return CHAREINK_16_FONT_ID;
#ifndef OMIT_XLARGE_FONT
        case EXTRA_LARGE:
          return CHAREINK_18_FONT_ID;
#endif
#ifndef OMIT_HUGE_FONT
        case HUGE_SIZE:
          return CHAREINK_20_FONT_ID;
#endif
      }
    case BITTER:
      switch (effectiveSize) {
#ifndef OMIT_TEENSY_FONT
        case TEENSY:
          return BITTER_8_FONT_ID;
#endif
#ifndef OMIT_TINY_FONT
        case TINY:
          return BITTER_10_FONT_ID;
#endif
#ifndef OMIT_SMALL_FONT
        case SMALL:
          return BITTER_12_FONT_ID;
#endif
        case MEDIUM:
        default:
          return BITTER_14_FONT_ID;
        case LARGE:
          return BITTER_16_FONT_ID;
#ifndef OMIT_XLARGE_FONT
        case EXTRA_LARGE:
          return BITTER_18_FONT_ID;
#endif
#ifndef OMIT_HUGE_FONT
        case HUGE_SIZE:
          return BITTER_20_FONT_ID;
#endif
      }
  }
}
