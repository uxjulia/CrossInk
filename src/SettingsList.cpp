#include "SettingsList.h"

#include "sync/DocumentMatching.h"

const std::vector<SettingInfo>& getBaseSettingsList() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v;
    // Reserve the maximum final size. Growing this process-lifetime vector
    // would otherwise leave it holding roughly twice the memory it needs.
    v.reserve(getBaseSettingsCapacity());
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
    add(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                            StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_MINIMAL, StrId::STR_THEME_DASHBOARD,
                           StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED, StrId::STR_THEME_LYRA_CAROUSEL,
                           StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_COVER_GRID},
                          "uiTheme", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_THEME::CLASSIC, CrossPointSettings::UI_THEME::MINIMAL,
                                CrossPointSettings::UI_THEME::DASHBOARD, CrossPointSettings::UI_THEME::LYRA,
                                CrossPointSettings::UI_THEME::LYRA_3_COVERS,
                                CrossPointSettings::UI_THEME::LYRA_CAROUSEL, CrossPointSettings::UI_THEME::ROUNDEDRAFF,
                                CrossPointSettings::UI_THEME::COVER_GRID}));
    add(SettingInfo::Toggle(StrId::STR_SWAP_LIBRARY_FILE_BROWSER, &CrossPointSettings::swapLibraryFileBrowser,
                            "swapLibraryFileBrowser", StrId::STR_CAT_DISPLAY));
    add(SettingInfo::Enum(StrId::STR_UI_SCALE, &CrossPointSettings::uiScale, {StrId::STR_SMALL, StrId::STR_LARGE},
                          "uiScale", StrId::STR_CAT_DISPLAY)
            .withEnumRawValues({CrossPointSettings::UI_SCALE_SMALL, CrossPointSettings::UI_SCALE_LARGE}));
    add(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA, &CrossPointSettings::libraryUseMetadata,
                            "libraryUseMetadata", StrId::STR_CAT_DISPLAY));
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
    add(SettingInfo::Value(StrId::STR_CHARACTER_SPACING, &CrossPointSettings::characterSpacing,
                           {0, CrossPointSettings::MAX_CHARACTER_SPACING, 1}, "characterSpacing",
                           StrId::STR_CAT_READER));
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
        {StrId::STR_JUSTIFY, StrId::STR_DIR_LEFT, StrId::STR_CENTER, StrId::STR_DIR_RIGHT, StrId::STR_BOOK_S_STYLE},
        "paragraphAlignment", StrId::STR_CAT_READER));
#if CROSSINK_SCALABLE_FONTS
    add(SettingInfo::Enum(StrId::STR_PUBLISHER_FONTS, &CrossPointSettings::publisherFonts,
                          {StrId::STR_OFF, StrId::STR_PUBLISHER_FONTS_ACCENTS, StrId::STR_PUBLISHER_FONTS_ALL},
                          "publisherFonts", StrId::STR_CAT_READER));
#endif
    add(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_IMAGE_GRAYSCALE, &CrossPointSettings::imageGrayscale, "imageGrayscale",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_DISABLE_TOUCHSCREEN, &CrossPointSettings::disableReaderTouchscreen,
                            "disableReaderTouchscreen", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FORCE_PARAGRAPH_INDENTS, &CrossPointSettings::forceParagraphIndents,
                            "forceParagraphIndents", StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Toggle(StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, "guideReadingEnabled",
                            StrId::STR_CAT_READER));
    add(SettingInfo::Enum(StrId::STR_INDEXING_METHOD, &CrossPointSettings::indexingMethod,
                          {StrId::STR_INDEXING_INCREMENTAL, StrId::STR_INDEXING_FULL_SECTION}, "indexingMethod",
                          StrId::STR_CAT_READER));

    // --- Controls ---
    add(SettingInfo::Toggle(StrId::STR_PINCH_FONT_RESIZE, &CrossPointSettings::pinchFontResizeEnabled,
                            "pinchFontResizeEnabled", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Toggle(StrId::STR_TWO_FINGER_ROTATION, &CrossPointSettings::twoFingerRotationEnabled,
                            "twoFingerRotationEnabled", StrId::STR_CAT_CONTROLS));
    const std::vector<StrId> twoFingerSwipeActions = {
        StrId::STR_NOT_SET,
        StrId::STR_INCREASE_BRIGHTNESS,
        StrId::STR_DECREASE_BRIGHTNESS,
        StrId::STR_INCREASE_WARMTH,
        StrId::STR_DECREASE_WARMTH,
        StrId::STR_NEXT_CHAPTER,
        StrId::STR_PREVIOUS_CHAPTER,
        StrId::STR_INCREASE_FONT_SIZE,
        StrId::STR_DECREASE_FONT_SIZE,
        StrId::STR_BACK_HOME,
        StrId::STR_HOME_READER,
        StrId::STR_SELECT_CHAPTER,
        StrId::STR_DISABLE_PAGE_TURN_GESTURES,
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
        CrossPointSettings::TWO_FINGER_SWIPE_BACK_HOME,
        CrossPointSettings::TWO_FINGER_SWIPE_HOME_READER,
        CrossPointSettings::TWO_FINGER_SWIPE_SELECT_CHAPTER,
        CrossPointSettings::TWO_FINGER_SWIPE_TOGGLE_PAGE_TURN_GESTURES,
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
#if defined(CROSSINK_APP_CAP_TOUCH) && CROSSINK_APP_CAP_TOUCH
    add(SettingInfo::Enum(StrId::STR_LEFT_EDGE_UP, &CrossPointSettings::leftEdgeUp, twoFingerSwipeActions, "leftEdgeUp",
                          StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_LEFT_EDGE_DOWN, &CrossPointSettings::leftEdgeDown, twoFingerSwipeActions,
                          "leftEdgeDown", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_RIGHT_EDGE_UP, &CrossPointSettings::rightEdgeUp, twoFingerSwipeActions,
                          "rightEdgeUp", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
    add(SettingInfo::Enum(StrId::STR_RIGHT_EDGE_DOWN, &CrossPointSettings::rightEdgeDown, twoFingerSwipeActions,
                          "rightEdgeDown", StrId::STR_CAT_CONTROLS)
            .withEnumRawValues(twoFingerSwipeActionValues));
#endif
    add(SettingInfo::Toggle(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::sideButtonOrientationAware,
                            "sideButtonOrientationAware", StrId::STR_CAT_CONTROLS));
    add(buildSideButtonActionSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::sideButtonUpShort,
                                     "sideButtonUpShort"));
    add(buildSideButtonActionSetting(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::sideButtonUpLong,
                                     "sideButtonUpLong"));
    add(buildSideButtonActionSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::sideButtonDownShort,
                                     "sideButtonDownShort"));
    add(buildSideButtonActionSetting(StrId::STR_LONG_PRESS_ACTION, &CrossPointSettings::sideButtonDownLong,
                                     "sideButtonDownLong"));
    add(SettingInfo::Enum(StrId::STR_ORIENTATION_AWARE, &CrossPointSettings::frontButtonOrientationAware,
                          {StrId::STR_NO, StrId::STR_NAV_BUTTONS, StrId::STR_ALL_BUTTONS},
                          "frontButtonOrientationAware", StrId::STR_CAT_CONTROLS));

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
    add(buildSideButtonActionSetting(StrId::STR_LONG_PRESS_LEFT_ACTION, &CrossPointSettings::frontButtonLeftLong,
                                     "frontButtonLeftLong"));
    add(buildSideButtonActionSetting(StrId::STR_LONG_PRESS_RIGHT_ACTION, &CrossPointSettings::frontButtonRightLong,
                                     "frontButtonRightLong"));
    add(buildShortcutSetting(StrId::STR_LONG_PRESS_BACK_ACTION, &CrossPointSettings::longPressBackAction,
                             "longPressBackAction", ShortcutOptionCatalog::LongPress));
    add(buildShortcutSetting(StrId::STR_LONG_PRESS_MENU_ACTION, &CrossPointSettings::longPressMenuAction,
                             "longPressMenuAction", ShortcutOptionCatalog::LongPress));
    add(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                            "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_NEXT_PAGE, &CrossPointSettings::pageTurnGesture,
                          {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                           StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                          "pageTurnGesture", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Enum(StrId::STR_PREV_PAGE, &CrossPointSettings::previousPageGesture,
                          {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                           StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                          "previousPageGesture", StrId::STR_CAT_CONTROLS));
    add(SettingInfo::Toggle(StrId::STR_TAP_HIDE_STATUS_BAR, &CrossPointSettings::tapToHideStatusBar,
                            "tapToHideStatusBar", StrId::STR_CAT_CONTROLS));

    add(SettingInfo::Enum(StrId::STR_MENU_NAVIGATION, &CrossPointSettings::menuNavigation,
                          {StrId::STR_MENU_DIRECTIONAL, StrId::STR_MENU_CLASSIC}, "menuNavigation",
                          StrId::STR_CAT_CONTROLS));

    // --- System ---
    add(SettingInfo::String(StrId::STR_DEVICE_NAME, SETTINGS.deviceName, sizeof(SETTINGS.deviceName), "deviceName",
                            StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Value(
        StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
        {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
        "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_CUSTOM_BOOTSCREEN, &CrossPointSettings::customBootscreenEnabled,
                            "customBootscreenEnabled", StrId::STR_CAT_SYSTEM));
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
    add(SettingInfo::String(StrId::STR_OPDS_DEFAULT_FOLDER, SETTINGS.opdsDownloadFolder,
                            sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
    // Persisted here, but edited from the nearby receive screen's folder picker.
    add(SettingInfo::String(StrId::STR_NEARBY_RECEIVE_FOLDER, SETTINGS.nearbyReceiveFolder,
                            sizeof(SETTINGS.nearbyReceiveFolder), "nearbyReceiveFolder"));
    add(SettingInfo::Value(StrId::STR_IDLE_TIME_THRESHOLD, &CrossPointSettings::readingIdleTimeThresholdUnits,
                           {CrossPointSettings::MIN_READING_IDLE_TIME_THRESHOLD_UNITS,
                            CrossPointSettings::MAX_READING_IDLE_TIME_THRESHOLD_UNITS, 1},
                           "readingIdleTimeThresholdUnits", StrId::STR_CAT_SYSTEM));
    add(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats, "trackReadingStats",
                            StrId::STR_CAT_SYSTEM));

    // Frontlight quick-panel state: persisted + web-exposed, category-less so
    // it stays off the Settings screen (edited from the swipe-down panel).
    add(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                           "frontlightBrightness"));
    add(SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
    add(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));

    // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
    add(SettingInfo::DynamicEnum(
        StrId::STR_SERVER_TYPE, {StrId::STR_CROSSPOINT, StrId::STR_KOSYNC_SERVER, StrId::STR_OTHER},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getServerType()); },
        [](uint8_t value) {
          KOREADER_STORE.setServerType(static_cast<KOReaderServerType>(value));
          KOREADER_STORE.saveToFile();
        },
        "koServerType", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicString(
        StrId::STR_CUSTOM_HEADER_1, [] { return formatHttpHeaderLine(KOREADER_STORE.getCustomHeaders()[0], true); },
        [](const std::string& value) {
          if (value == formatHttpHeaderLine(KOREADER_STORE.getCustomHeaders()[0], true)) return;
          HttpHeader header;
          if (parseHttpHeaderLine(value, header) && KOREADER_STORE.setCustomHeader(0, header.name, header.value))
            KOREADER_STORE.saveToFile();
        },
        "koCustomHeader1", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicString(
        StrId::STR_CUSTOM_HEADER_2, [] { return formatHttpHeaderLine(KOREADER_STORE.getCustomHeaders()[1], true); },
        [](const std::string& value) {
          if (value == formatHttpHeaderLine(KOREADER_STORE.getCustomHeaders()[1], true)) return;
          HttpHeader header;
          if (parseHttpHeaderLine(value, header) && KOREADER_STORE.setCustomHeader(1, header.name, header.value))
            KOREADER_STORE.saveToFile();
        },
        "koCustomHeader2", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
        },
        "koUsername", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicString(
        StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
        [](const std::string& v) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
          KOREADER_STORE.saveToFile();
        },
        "koPassword", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicString(
        StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
        [](const std::string& v) {
          KOREADER_STORE.setServerUrl(v);
          KOREADER_STORE.saveToFile();
        },
        "koServerUrl", StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicEnum(
        StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
        // The web settings page warns before saving a change.
        [](uint8_t v) { DocumentMatching::set(static_cast<DocumentMatchMethod>(v)); }, "koMatchMethod",
        StrId::STR_SYNC_SERVER));
    add(SettingInfo::DynamicEnum(
        StrId::STR_SEND_METADATA, {StrId::STR_OFF, StrId::STR_ON},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
        [](uint8_t v) {
          KOREADER_STORE.setSendMetadata(v != 0);
          KOREADER_STORE.saveToFile();
        },
        "koSendMetadata", StrId::STR_SYNC_SERVER));

    add(SettingInfo::DynamicEnum(
        StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
        [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
        [](uint8_t v) {
          KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
          KOREADER_STORE.saveToFile();
        },
        "koSyncBehavior", StrId::STR_SYNC_SERVER));

    // Legacy fields stay in JSON for one-time status bar migration; the web
    // editor uses /api/status-bars instead of exposing these controls.
    add(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_STATUS_BARS));
    add(SettingInfo::Toggle(StrId::STR_STABLE_PAGE_NUMBERS, &CrossPointSettings::stablePageNumbers, "stablePageNumbers",
                            StrId::STR_STATUS_BARS));
    add(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_PERCENTAGE_FORMAT, &CrossPointSettings::statusBarBookPercentageFormat,
                          {StrId::STR_PERCENTAGE_FORMAT_WHOLE, StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                           StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS},
                          "statusBarBookPercentageFormat", StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarProgressBar",
                          StrId::STR_STATUS_BARS)
            .withEnumRawValues({CrossPointSettings::HIDE_PROGRESS, CrossPointSettings::BOOK_PROGRESS,
                                CrossPointSettings::CHAPTER_PROGRESS}));
    add(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER}, "statusBarTitle",
                          StrId::STR_STATUS_BARS)
            .withEnumRawValues(
                {CrossPointSettings::HIDE_TITLE, CrossPointSettings::BOOK_TITLE, CrossPointSettings::CHAPTER_TITLE}));
    add(SettingInfo::Enum(StrId::STR_TIME_LEFT, &CrossPointSettings::statusBarTimeLeft,
                          {StrId::STR_HIDE, StrId::STR_CHAPTER, StrId::STR_BOOK}, "statusBarTimeLeft",
                          StrId::STR_STATUS_BARS));
    add(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                          {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP, StrId::STR_STATUS_BAR_BOTH},
                          "xtcStatusBarMode", StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_STATUS_BAR_TEXT_SIZE, &CrossPointSettings::statusBarTextSize,
                          {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}, "statusBarTextSize",
                          StrId::STR_STATUS_BARS));
    add(SettingInfo::Enum(StrId::STR_STATUS_BAR_TEXT_SIZE, &CrossPointSettings::displayStatusBarTextSize,
                          {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}, "displayStatusBarTextSize",
                          StrId::STR_CAT_DISPLAY));
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
    // Keep persisted preferences even when the IMU fails its startup probe.
    {
      auto shortPowerButtonIt = std::find_if(
          v.begin(), v.end(), [](const SettingInfo& setting) { return settingKeyIs(setting, "shortPwrBtn"); });
      if (shortPowerButtonIt != v.end()) {
        auto insertPos = v.insert(shortPowerButtonIt + 1,
                                  SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                                      "tiltPageTurn", StrId::STR_CAT_CONTROLS));
        insertPos = v.insert(
            insertPos + 1,
            SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN_DIRECTION, &CrossPointSettings::tiltPageTurnDirection,
#if CROSSINK_APP_DEVICE_X4CLASSIC || defined(SIMULATOR_DEVICE_X4_CLASSIC)
                              // X4 Classic's X-axis has the opposite sign from the original X3 calibration.
                              // Keep the stored direction, but name its physical motion accurately.
                              {StrId::STR_TILT_DIRECTION_LEFT_RIGHT_INVERTED, StrId::STR_TILT_DIRECTION_LEFT_RIGHT,
                               StrId::STR_TILT_DIRECTION_FORWARD_BACK, StrId::STR_TILT_DIRECTION_FORWARD_BACK_INVERTED},
#else
                              {StrId::STR_TILT_DIRECTION_LEFT_RIGHT, StrId::STR_TILT_DIRECTION_LEFT_RIGHT_INVERTED,
                               StrId::STR_TILT_DIRECTION_FORWARD_BACK, StrId::STR_TILT_DIRECTION_FORWARD_BACK_INVERTED},
#endif
                              "tiltPageTurnDirection", StrId::STR_CAT_CONTROLS));
        v.insert(insertPos + 1,
                 SettingInfo::Toggle(StrId::STR_TILT_NEXT_PAGE_ONLY, &CrossPointSettings::tiltNextPageOnly,
                                     "tiltNextPageOnly", StrId::STR_CAT_CONTROLS));
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
