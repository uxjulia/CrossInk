#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    OVERLAY = 6,
    READING_STATS_SLEEP = 7,
    MINIMAL_SLEEP = 8,
    QUICK_RESUME = 9,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum STATUS_BAR_TIME_LEFT {
    TIME_LEFT_HIDE = 0,
    TIME_LEFT_CHAPTER = 1,
    TIME_LEFT_BOOK = 2,
    STATUS_BAR_TIME_LEFT_COUNT
  };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTON_LAYOUT_COUNT };

  enum FRONT_BUTTON_ORIENTATION_AWARE {
    FRONT_ORIENTATION_AWARE_OFF = 0,
    FRONT_ORIENTATION_AWARE_NAV_BUTTONS = 1,
    FRONT_ORIENTATION_AWARE_ALL_BUTTONS = 2,
    FRONT_ORIENTATION_AWARE_COUNT
  };

  // Side button long-press action options
  enum SIDE_LONG_PRESS {
    SIDE_LONG_CHAPTER_SKIP = 0,
    SIDE_LONG_FONT_SIZE = 1,
    SIDE_LONG_OFF = 2,
    SIDE_LONG_ORIENTATION_CHANGE = 3,
    SIDE_LONG_PRESS_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { LEXENDDECA = 0, BITTER = 1, CHAREINK = 2, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE {
    TINY = 0,
    SMALL = 1,
    MEDIUM = 2,
    LARGE = 3,
    EXTRA_LARGE = 4,
    TEENSY = 5,
    HUGE_SIZE = 6,
    ITTY_BITTY = 7,
    FONT_SIZE_COUNT
  };
  enum SD_FONT_SIZE_RANGE {
    SD_FONT_RANGE_TEENSY = 0,
    SD_FONT_RANGE_TINY = 1,
    SD_FONT_RANGE_XLARGE = 2,
    SD_FONT_RANGE_NO_EMOJI = 3,
    SD_FONT_RANGE_ALL = 4,
    SD_FONT_SIZE_RANGE_COUNT
  };
  // Legacy persisted values for the old Tight / Normal / Wide line-spacing setting.
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_3_MIN = 5,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    TOGGLE_FONT = 4,
    TOGGLE_GUIDE_DOTS = 5,
    TOGGLE_BIONIC_READING = 6,
    TOGGLE_BOOKMARK = 7,
    SYNC_PROGRESS = 8,
    MARK_FINISHED = 9,
    READING_STATS = 10,
    SCREENSHOT = 11,
    CYCLE_PAGE_TURN = 12,
    FILE_TRANSFER = 13,
    TOGGLE_TILT_PAGE_TURN = 14,
    SHORT_PWRBTN_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme. Raw values are persisted in settings; keep existing values stable.
  enum UI_THEME {
    CLASSIC = 0,
    LYRA = 1,
    LYRA_3_COVERS = 2,
    ROUNDEDRAFF = 3,
    LYRA_CAROUSEL = 4,
    MINIMAL = 5,
    UI_THEME_COUNT = 6
  };
  enum RECENT_BOOKS_VIEW { RECENT_BOOKS_LIST = 0, RECENT_BOOKS_GRID = 1, RECENT_BOOKS_VIEW_COUNT };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_ON = 1, TILT_PAGE_TURN_COUNT };
  enum TILT_PAGE_TURN_DIRECTION {
    TILT_LEFT_RIGHT = 0,
    TILT_LEFT_RIGHT_INVERTED = 1,
    TILT_FORWARD_BACK = 2,
    TILT_FORWARD_BACK_INVERTED = 3,
    TILT_PAGE_TURN_DIRECTION_COUNT
  };

  // Long-press Confirm (menu button) quick action in reader
  enum LONG_PRESS_MENU_ACTION {
    LONG_MENU_OFF = 0,
    LONG_MENU_SLEEP = 1,
    LONG_MENU_CHANGE_FONT = 2,
    LONG_MENU_TOGGLE_GUIDE_DOTS = 3,
    LONG_MENU_TOGGLE_BIONIC = 4,
    LONG_MENU_TOGGLE_BOOKMARK = 5,
    LONG_MENU_REFRESH_SCREEN = 6,
    LONG_MENU_SYNC_PROGRESS = 7,
    LONG_MENU_MARK_FINISHED = 8,
    LONG_MENU_READING_STATS = 9,
    LONG_MENU_SCREENSHOT = 10,
    LONG_MENU_CYCLE_PAGE_TURN = 11,
    LONG_MENU_FILE_TRANSFER = 12,
    LONG_MENU_TOGGLE_TILT_PAGE_TURN = 13,
    LONG_PRESS_MENU_ACTION_COUNT
  };

  // Clipping storage mode
  enum CLIPPING_STORAGE : uint8_t { SINGLE_FILE = 0, PER_BOOK = 1, CLIPPING_STORAGE_COUNT };
  // Clip selector navigation scheme
  enum CLIP_NAV_MODE : uint8_t { LINE_AWARE = 0, WORD_BY_WORD = 1, CLIP_NAV_MODE_COUNT };
  // Annotation underline visibility
  enum ANNOTATION_VISIBILITY : uint8_t { ANNOT_VISIBLE = 0, ANNOT_HIDDEN = 1, ANNOTATION_VISIBILITY_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarTimeLeft = TIME_LEFT_HIDE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = 0;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t textAntiAliasing = 1;
  // Short power button action behaviour
  uint8_t shortPwrBtn = IGNORE;
  // Long power button action behaviour
  uint8_t longPwrBtn = SLEEP;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonOrientationAware = FRONT_ORIENTATION_AWARE_OFF;
  uint8_t sideButtonOrientationAware = 0;
  // Action performed when side buttons are long-pressed in reader
  uint8_t sideButtonLongPress = SIDE_LONG_CHAPTER_SKIP;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader-specific front button remap (overrides system mapping while in reader activities).
  // readerFrontButtonsEnabled = 0 means the reader uses the system mapping above.
  uint8_t readerFrontButtonsEnabled = 0;
  uint8_t readerFrontButtonBack = FRONT_HW_BACK;
  uint8_t readerFrontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t readerFrontButtonLeft = FRONT_HW_LEFT;
  uint8_t readerFrontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = LEXENDDECA;
  uint8_t fontSize = MEDIUM;
#if defined(OMIT_EMOJI_FONTS)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_NO_EMOJI;
#elif defined(OMIT_TINY_FONT) && defined(OMIT_SMALL_FONT)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_XLARGE;
#elif defined(OMIT_MEDIUM_FONT) && defined(OMIT_LARGE_FONT) && defined(OMIT_XLARGE_FONT) && defined(OMIT_HUGE_FONT)
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TEENSY;
#else
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TINY;
#endif
  uint8_t lineSpacing = NORMAL;  // migration only; new saves use lineHeightPercent
  uint8_t lineHeightPercent = 100;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Recent Books screen layout
  uint8_t recentBooksView = RECENT_BOOKS_LIST;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t bionicReadingEnabled = 0;
  // Guide Dots - places a middle dot between words to guide the eye
  uint8_t guideReadingEnabled = 0;
  // SD card font family name, including optional range suffix (empty = use built-in fontFamily)
  char sdFontFamilyName[64] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when marked as finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Long-press Confirm (menu button) quick action in reader (0 = off)
  uint8_t longPressMenuAction = LONG_MENU_OFF;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  uint8_t tiltPageTurnDirection = TILT_LEFT_RIGHT;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 400;
  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = 30;
  static constexpr uint8_t SD_FONT_MAX_SIZE_STEPS = 8;
  static constexpr uint8_t MIN_LINE_HEIGHT_PERCENT = 70;
  static constexpr uint8_t MAX_LINE_HEIGHT_PERCENT = 200;
  static constexpr uint8_t LINE_HEIGHT_PERCENT_STEP = 1;

  uint16_t getPowerButtonWakeDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? POWER_BUTTON_WAKE_SHORT_MS
                                                                    : POWER_BUTTON_LONG_PRESS_MS;
  }

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const { return getPowerButtonWakeDuration(); }
  uint16_t getPowerButtonLongPressDuration() const { return POWER_BUTTON_LONG_PRESS_MS; }
  static uint8_t getActiveReaderFontSizeCount();
  static uint8_t getStoredReaderFontSize(FONT_SIZE size);
  static uint8_t getReaderFontPointSize(FONT_SIZE size);
  static uint8_t getSdFontRangePointSize(uint8_t range, uint8_t step);
  static bool isSdFontPointSizeAllowedForRange(uint8_t pointSize, uint8_t range);
  FONT_SIZE getEffectiveReaderFontSize() const;
  uint8_t getSdFontTargetPointSize() const;
  bool changeReaderFontSize(bool larger);
  int getReaderFontId() const;
  int getBuiltInReaderFontId() const;

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(FsFile& file, bool count_only = false) const;

  bool saveToFile() const;
  bool loadFromFile();

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static void validateReaderFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);
  static uint8_t sleepScreenStorageToMode(uint8_t storedValue);
  static uint8_t sleepScreenModeToStorage(uint8_t mode);
  static uint8_t legacyLineSpacingToPercent(uint8_t legacyValue, uint8_t fontFamily, bool sdFontSelected);
  static uint8_t clampedLineHeightPercent(uint8_t value);
#ifdef SIMULATOR
  static bool verifySleepTimeoutMigrationContract();
  static bool verifySleepScreenMigrationContract();
#endif

 private:
  bool loadFromBinaryFile();
  bool migrateLanguageBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
