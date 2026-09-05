#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>

#include "ReaderFontSizeStep.h"

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  mutable std::mutex _mutex;

  CrossPointSettings() = default;
  friend class PersistableStore<CrossPointSettings>;

 public:
  // Access the settings mutex for protecting multi-field reads/writes from other cores.
  // Callers must not re-enter SETTINGS methods that lock _mutex while holding it.
  std::mutex& getMutex() const { return _mutex; }

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
    MINIMAL_STATS_SLEEP = 10,
    DASHBOARD_SLEEP = 11,
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
  enum HIDE_CLOCK_MODE { HIDE_CLOCK_NEVER = 0, HIDE_CLOCK_IN_READER = 1, HIDE_CLOCK_ALWAYS = 2, HIDE_CLOCK_MODE_COUNT };
  // Persisted date-format values mirror HalClock::DateFormat.
  enum DATE_FORMAT {
    DATE_FORMAT_MONTH_DAY_YEAR_LONG = 0,
    DATE_FORMAT_DAY_MONTH_YEAR_LONG = 1,
    DATE_FORMAT_MONTH_DAY_YEAR_NUMERIC = 2,
    DATE_FORMAT_DAY_MONTH_YEAR_NUMERIC = 3,
    DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC = 4,
    DATE_FORMAT_MONTH_DAY_NUMERIC = 5,
    DATE_FORMAT_DAY_MONTH_NUMERIC = 6,
    DATE_FORMAT_MONTH_DAY_LONG = 7,
    DATE_FORMAT_DAY_MONTH_LONG = 8,
    DATE_FORMAT_COUNT
  };
  enum DATE_SEPARATOR {
    DATE_SEPARATOR_PERIOD = 0,
    DATE_SEPARATOR_HYPHEN = 1,
    DATE_SEPARATOR_SLASH = 2,
    DATE_SEPARATOR_COUNT
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
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    SIDE_BUTTON_LAYOUT_COUNT
  };

  enum FRONT_BUTTON_ORIENTATION_AWARE {
    FRONT_ORIENTATION_AWARE_OFF = 0,
    FRONT_ORIENTATION_AWARE_NAV_BUTTONS = 1,
    FRONT_ORIENTATION_AWARE_ALL_BUTTONS = 2,
    FRONT_ORIENTATION_AWARE_COUNT
  };

  enum TWO_FINGER_SWIPE_ACTION {
    TWO_FINGER_SWIPE_NOT_SET = 0,
    TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS,
    TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS,
    TWO_FINGER_SWIPE_INCREASE_WARMTH,
    TWO_FINGER_SWIPE_DECREASE_WARMTH,
    TWO_FINGER_SWIPE_NEXT_CHAPTER,
    TWO_FINGER_SWIPE_PREVIOUS_CHAPTER,
    TWO_FINGER_SWIPE_INCREASE_FONT_SIZE,
    TWO_FINGER_SWIPE_DECREASE_FONT_SIZE,
    TWO_FINGER_SWIPE_ACTION_COUNT,
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
  enum FONT_FAMILY { LEXENDDECA = 0, BITTER = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { TINY = 0, SMALL = 1, MEDIUM = 2, LARGE = 3, FONT_SIZE_COUNT };
  enum SD_FONT_SIZE_RANGE {
    SD_FONT_RANGE_TEENSY = 0,
    SD_FONT_RANGE_TINY = 1,
    SD_FONT_RANGE_XLARGE = 2,
    SD_FONT_RANGE_NO_EMOJI_LEGACY = 3,
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
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  enum FILE_BROWSER_DISPLAY {
    FILE_BROWSER_DISPLAY_1_LINE = 0,
    FILE_BROWSER_DISPLAY_2_LINES = 1,
    FILE_BROWSER_DISPLAY_COUNT
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
    TOGGLE_DARK_MODE = 15,
    FOOTNOTES = 16,
    FILE_BROWSER = 17,
    CALIBRE_WIRELESS = 18,
    JOIN_NETWORK = 19,
    CREATE_HOTSPOT = 20,
    CREATE_CLIPPING = 21,
    LOOKUP_WORD = 22,
    // Values 23-26 are already persisted by the X4 Pro Home-key feature.
    // Keep Quick Actions separate so existing Home-key mappings retain their meaning.
    TOGGLE_HOME_BUTTON_IN_READER = 26,
    QUICK_ACTIONS = 27,
    TOGGLE_FRONTLIGHT = 28,
    TOGGLE_TOUCHSCREEN = 29,
    // Appended after the X4 Pro and Quick Actions values so existing settings
    // files continue to mean exactly the same thing.
    QUICK_LOCK = 30,
    // Shortcut values are persisted. Append new actions; never reuse removed
    // raw values or they can silently change an existing binding's behavior.
    PREVIOUS_PAGE = 31,
    NEARBY_POSITION_SYNC = 32,
    SHORT_PWRBTN_COUNT
  };

  // Power + Up side-button chord actions. Keep this order aligned with
  // ButtonShortcutController::ChordAction because the runtime casts the
  // persisted value to that enum.
  enum POWER_CHORD_ACTION {
    CHORD_SCREENSHOT = 0,
    CHORD_QUICK_LOCK = 1,
    // Values 2 and 3 were removed Next Page and Previous Page actions. Keep
    // them unused so an interim settings file cannot remap them to another action.
    CHORD_DISABLED = 4,
    CHORD_SLEEP = 5,
    CHORD_PAGE_TURN = 6,
    CHORD_TOGGLE_BOOKMARK = 7,
    CHORD_READING_STATS = 8,
    CHORD_MARK_FINISHED = 9,
    CHORD_FORCE_REFRESH = 10,
    CHORD_TOGGLE_FONT = 11,
    CHORD_TOGGLE_GUIDE_DOTS = 12,
    CHORD_TOGGLE_BIONIC_READING = 13,
    CHORD_CYCLE_PAGE_TURN = 14,
    CHORD_SYNC_PROGRESS = 15,
    CHORD_FILE_TRANSFER = 16,
    CHORD_CALIBRE_WIRELESS = 17,
    CHORD_JOIN_NETWORK = 18,
    CHORD_CREATE_HOTSPOT = 19,
    CHORD_TOGGLE_DARK_MODE = 20,
    CHORD_FOOTNOTES = 21,
    CHORD_FILE_BROWSER = 22,
    CHORD_CREATE_CLIPPING = 23,
    CHORD_LOOKUP_WORD = 24,
    CHORD_TOGGLE_HOME_BUTTON = 25,
    CHORD_QUICK_ACTIONS = 26,
    CHORD_TOGGLE_FRONTLIGHT = 27,
    CHORD_TOGGLE_TOUCHSCREEN = 28,
    CHORD_PREVIOUS_PAGE = 29,
    CHORD_NEARBY_POSITION_SYNC = 30,
    POWER_CHORD_ACTION_COUNT
  };

  // Home-key shortcuts reuse power-button actions where possible. Keep the
  // dedicated values stable because they are persisted in settings.bin.
  enum HOME_BUTTON_ACTION {
    HOME_BUTTON_BACK_HOME = 23,
    HOME_BUTTON_TOGGLE_FRONTLIGHT = 24,
    HOME_BUTTON_READER_MENU = 25,
    HOME_BUTTON_ACTION_COUNT = 26
  };

  static constexpr uint8_t QUICK_ACTION_SLOT_ACTION_COUNT = 23;

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    FONT_SIZE_CHANGE = 3,
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
    DASHBOARD = 6,
    UI_THEME_COUNT = 7
  };
  enum RECENT_BOOKS_VIEW { RECENT_BOOKS_LIST = 0, RECENT_BOOKS_GRID = 1, RECENT_BOOKS_VIEW_COUNT };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };
  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };
  enum PAGE_TURN_GESTURE {
    TAP_AND_SWIPE = 0,
    TAP_ONLY = 1,
    SWIPE_ONLY = 2,
    INVERTED_TAP = 3,
    PAGE_TURN_GESTURE_DISABLED = 4,
    PAGE_TURN_GESTURE_COUNT
  };

  enum INDEXING_METHOD { INDEXING_INCREMENTAL = 0, INDEXING_FULL_SECTION = 1, INDEXING_METHOD_COUNT };

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
    LONG_MENU_TOGGLE_DARK_MODE = 14,
    LONG_MENU_FOOTNOTES = 15,
    LONG_MENU_FILE_BROWSER = 16,
    LONG_MENU_CALIBRE_WIRELESS = 17,
    LONG_MENU_JOIN_NETWORK = 18,
    LONG_MENU_CREATE_HOTSPOT = 19,
    LONG_MENU_CREATE_CLIPPING = 20,
    LONG_MENU_LOOKUP_WORD = 21,
    // Appended: values are persisted in settings.bin.
    LONG_MENU_QUICK_ACTIONS = 22,
    LONG_MENU_QUICK_LOCK = 23,
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

  // UI scale for list-style screens: sizes list fonts and row heights
  // together so touch targets grow uniformly.
  enum UI_SCALE { UI_SCALE_SMALL = 0, UI_SCALE_LARGE = 1, UI_SCALE_COUNT };
  static uint8_t defaultUiScale();

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per render
  // by ActivityManager. Quick Resume preserves it; other sleep screens remain normal.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t stablePageNumbers = 0;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarTimeLeft = TIME_LEFT_HIDE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock visibility mode (requires an RTC-backed clock).
  uint8_t hideClock = HIDE_CLOCK_ALWAYS;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Date display format. Values match HalClock::DateFormat; 0 preserves the existing "Jan 01, 2026" default.
  uint8_t dateFormat = DATE_FORMAT_MONTH_DAY_YEAR_LONG;
  // Separator for numeric dates. Text-based date formats do not use it.
  uint8_t dateSeparator = DATE_SEPARATOR_SLASH;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Set once an NTP sync writes both date and time. Kept separate so older
  // time-only syncs do not unlock date display with stale RTC date registers.
  uint8_t clockDateHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t textAntiAliasing = 1;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Page-turn gestures remain independently configurable while touch reader controls stay enabled.
  uint8_t pageTurnGesture = TAP_AND_SWIPE;
  // Disables all touchscreen input while a reader is active. Reader menus temporarily override this.
  uint8_t disableReaderTouchscreen = 0;
  // Available only on multi-touch hardware; defaults on for pinch font resizing.
  uint8_t pinchFontResizeEnabled = 1;
  // Configurable two-finger swipes. A non-empty action may be assigned to one direction only.
  uint8_t twoFingerSwipeUp = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeDown = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeLeft = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeRight = TWO_FINGER_SWIPE_NOT_SET;
  // Short power button action behaviour
  uint8_t shortPwrBtn = IGNORE;
  // Long power button action behaviour
  uint8_t longPwrBtn = SLEEP;
  // Power + Up shortcut action. Disabled by default so the established
  // Power + Down screenshot chord remains screenshot-only.
  uint8_t powerChordAction = CHORD_DISABLED;
  // Up + Down shortcut action. On touch hardware, while the reader touchscreen
  // is disabled, this chord instead opens Settings as the recovery route.
  uint8_t sideButtonChordAction = CHORD_DISABLED;
  // X4 Pro capacitive Home-key actions. Values below SHORT_PWRBTN_COUNT map
  // directly to the matching power-button shortcut action.
  uint8_t homeButtonTapAction = HOME_BUTTON_BACK_HOME;
  uint8_t homeButtonDoubleTapAction = HOME_BUTTON_TOGGLE_FRONTLIGHT;
  uint8_t homeButtonLongPressAction = HOME_BUTTON_READER_MENU;
  // Home-key devices can lock the capacitive Home key while a reader page is
  // active. Reader menus temporarily override this without changing the value.
  uint8_t homeButtonInReaderEnabled = 1;
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
  // The physical reader size selected by the user. Built-in and SD-card font
  // families resolve this to their closest available file.
  uint8_t readerFontPointSize = 14;
  // Transient compatibility state for JSON settings written before fontSize
  // stored a point size. SdCardFontSystem resolves it once the family catalog
  // is available, then persists readerFontPointSize.
  uint8_t legacySdFontSizeStep = UINT8_MAX;
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TINY;
  uint8_t lineSpacing = NORMAL;  // migration only; new saves use lineHeightPercent
  uint8_t lineHeightPercent = 100;
  uint8_t wordSpacing = 0;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margins. Legacy single-axis settings initialize both values.
  uint8_t screenMarginVertical = 5;
  uint8_t screenMarginHorizontal = 5;
  // Show EPUB publisher pagebreak labels in the reader margin when present.
  uint8_t publisherPageNumbers = 0;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // OPDS download destination (empty = SD root). Edited from the OPDS server list.
  char opdsDownloadFolder[64] = "";
  // Nearby file receive destination (empty = SD root).
  char nearbyReceiveFolder[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Recent Books screen layout
  uint8_t recentBooksView = RECENT_BOOKS_LIST;
  // UI scale (list fonts + row heights); touch boards default one step larger
  uint8_t uiScale = defaultUiScale();
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Quick-return from footnotes when a footnote shortcut is active.
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // EPUB section indexing policy. The current chapter keeps its active build.
  uint8_t indexingMethod = INDEXING_FULL_SECTION;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t bionicReadingEnabled = 0;
  // Guide Dots - places a middle dot between words to guide the eye
  uint8_t guideReadingEnabled = 0;
  // Per-book EPUB render mode runtime value. This is intentionally not saved as a global setting.
  uint8_t epubRenderMode = 0;
  // SD card font family name, including optional range suffix (empty = use built-in fontFamily)
  char sdFontFamilyName[64] = "";
  // Global dictionary SD-card font (empty = use the reader font).
  char dictionarySdFontFamilyName[64] = "";
  // Zero follows the active reader size.
  uint8_t dictionaryFontPointSize = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Hide file extensions in the file browser right-side value column (0 = show, 1 = hide)
  uint8_t hideFileExtension = 0;
  // File browser display row style (0 = one-line theme list, 1 = two-line compact display)
  uint8_t fileBrowserDisplay = FILE_BROWSER_DISPLAY_1_LINE;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when marked as finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Automatically write a dated global reading-stats backup before sleep when an RTC is available (0 = off, 1 = on).
  uint8_t autoBackupStats = 1;
  // Idle threshold for reading stats, stored in 10-second units to fit uint8_t.
  uint8_t readingIdleTimeThresholdUnits = 30;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Long-press Confirm (menu button) quick action in reader (0 = off)
  uint8_t longPressMenuAction = LONG_MENU_OFF;
  // Long-press Back quick action in reader (defaults to the historical file browser shortcut)
  uint8_t longPressBackAction = LONG_MENU_FILE_BROWSER;
  // Five reusable reader commands and their single owning shortcut. Keep these
  // adjacent so old settings files simply retain their default-initialized tail.
  uint8_t quickActionSlots[5] = {IGNORE, IGNORE, IGNORE, IGNORE, IGNORE};
  uint8_t quickActionsTrigger = 0;
  // Tilt-based page turning on devices with a supported IMU (X3 and Sticky).
  uint8_t tiltPageTurn = TILT_OFF;
  uint8_t tiltPageTurnDirection = TILT_LEFT_RIGHT;
  // Frontlight quick-panel state (boards with FREEINK_CAP_FRONTLIGHT, e.g. X4
  // Pro). Applied at boot, edited only from the frontlight panel; persisted as
  // category-less entries so they stay out of the Settings screen. Writes are
  // debounced by the panel (saved once on exit), not per slider tick.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // When enabled, restore a previously-on light after sleep. A previous off
  // state falls through to a complete schedule.
  uint8_t frontlightRestoreOnWake = 1;
  // Daily wake-only schedule, in local minutes since midnight.
  // An unset endpoint keeps the schedule inactive; its value is retained while
  // the schedule toggle is off so it can be re-enabled without re-entry.
  uint8_t frontlightScheduleEnabled = 0;
  uint16_t frontlightScheduleStart = 0xFFFF;
  uint16_t frontlightScheduleEnd = 0xFFFF;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Enabled keyboard layouts. Zero derives a default from the UI language;
  // non-zero bits follow KeyboardLayoutSet::ALL table order.
  uint16_t keyboardLayouts = 0;
  // Custom KOReader sync device display name. Empty means use the hardware default.
  char deviceName[21] = "";
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;
#ifdef CROSSINK_ENABLE_READING_STATS_TOGGLE
  // Debug/test builds can disable stat writes so navigation tests do not affect personal reading stats.
  uint8_t trackReadingStats = 1;
#endif

  ~CrossPointSettings() = default;

  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_WAKE_LONG_MS = 200;
  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 400;
  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;
  static constexpr uint8_t SD_FONT_MAX_SIZE_STEPS = 8;
  static constexpr uint8_t MIN_READER_FONT_POINT_SIZE = 8;
  static constexpr uint8_t MIN_LINE_HEIGHT_PERCENT = 70;
  static constexpr uint8_t MAX_LINE_HEIGHT_PERCENT = 200;
  static constexpr uint8_t LINE_HEIGHT_PERCENT_STEP = 1;
  static constexpr uint8_t MIN_SCREEN_MARGIN = 5;
  static constexpr uint8_t MAX_SCREEN_MARGIN = 150;
  static constexpr uint8_t SCREEN_MARGIN_SMALL_STEP = 1;
  static constexpr uint8_t SCREEN_MARGIN_LARGE_STEP = 5;
  static constexpr uint8_t MAX_WORD_SPACING = 4;
  static constexpr uint16_t DEFAULT_READING_IDLE_TIME_THRESHOLD_SECONDS = 5 * 60;
  static constexpr uint16_t MIN_READING_IDLE_TIME_THRESHOLD_SECONDS = 30;
  static constexpr uint16_t MAX_READING_IDLE_TIME_THRESHOLD_SECONDS = 10 * 60;
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS = 10;
  static constexpr uint8_t MIN_READING_IDLE_TIME_THRESHOLD_UNITS =
      MIN_READING_IDLE_TIME_THRESHOLD_SECONDS / READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
  static constexpr uint8_t MAX_READING_IDLE_TIME_THRESHOLD_UNITS =
      MAX_READING_IDLE_TIME_THRESHOLD_SECONDS / READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
  static constexpr size_t MIN_DEVICE_NAME_LENGTH = 2;
  static constexpr size_t MAX_DEVICE_NAME_LENGTH = sizeof(deviceName) - 1;

  uint16_t getPowerButtonWakeDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? POWER_BUTTON_WAKE_SHORT_MS
                                                                    : POWER_BUTTON_WAKE_LONG_MS;
  }

  bool shouldShowClockInReader() const { return hideClock == HIDE_CLOCK_NEVER; }
  bool shouldShowClockOutsideReader() const {
    return hideClock == HIDE_CLOCK_NEVER || hideClock == HIDE_CLOCK_IN_READER;
  }
  bool shouldTrackReadingStats() const {
#ifdef CROSSINK_ENABLE_READING_STATS_TOGGLE
    return trackReadingStats != 0;
#else
    return true;
#endif
  }
  static const char* getDefaultDeviceName();
  const char* getEffectiveDeviceName() const;
  uint16_t getReadingIdleTimeThresholdSeconds() const;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t pointSize);
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
  bool changeReaderFontSize(bool larger, FontSizeStepMode mode = FontSizeStepMode::Wrap);
  int getReaderFontId() const;
  int getBuiltInReaderFontId() const;

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(HalFile& file, bool count_only = false) const;

  bool saveToFile() const;
  bool loadFromFile();
  static const char* getFilePath() { return "/.crosspoint/crossink-settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    bool showStablePageNumbers = false;
    uint8_t titleMode = HIDE_TITLE;
    uint8_t timeLeftMode = TIME_LEFT_HIDE;
    bool showBattery = false;
    bool showBatteryPercent = false;
    bool showClock = false;
    uint8_t progressBarMode = HIDE_PROGRESS;
    uint8_t progressBarHeightPx = 0;
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;

    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showStablePageNumbers || titleMode != HIDE_TITLE ||
             timeLeftMode != TIME_LEFT_HIDE || showBattery || (showClock && clockAvailable);
    }
    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
  };

  StatusBarSpec statusBarSpec() const;
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight,
                                    EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault) const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static void validateReaderFrontButtonMapping(CrossPointSettings& settings);
  static bool isTwoFingerSwipeActionAvailable(uint8_t action, bool frontlightPresent, bool hasColorTemperature);
  static bool normalizeTwoFingerSwipeActions(CrossPointSettings& settings,
                                             uint8_t CrossPointSettings::* editedField = nullptr);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);
  static uint8_t sleepScreenStorageToMode(uint8_t storedValue);
  static uint8_t sleepScreenModeToStorage(uint8_t mode);
  static uint8_t legacyLineSpacingToPercent(uint8_t legacyValue, uint8_t fontFamily, bool sdFontSelected);
  static uint8_t clampedLineHeightPercent(uint8_t value);
  static uint8_t readingIdleTimeThresholdUnitsForSeconds(uint16_t seconds);
  static uint16_t readingIdleTimeThresholdSecondsForUnits(uint8_t units);
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
