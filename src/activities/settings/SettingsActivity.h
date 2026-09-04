#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING, SECTION_HEADER, SUBMENU };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsReader,
  CustomiseStatusBar,
  KOReaderSync,
  OPDSBrowser,
  DisplaySleepScreen,
  DisplayFrontlight,
  ReaderFontOptions,
  ReaderPageLayout,
  ScreenMargin,
  QuickActions,
  ControlsPowerButton,
  ControlsHomeButton,
  ControlsFrontButtons,
  ControlsSideButtons,
  ControlsTapsGestures,
  ControlsTwoFingerSwipe,
  SystemDevice,
  SystemFilesCache,
  SystemReadingStats,
  SystemGlobalStats,
  Network,
  BackupStats,
  ResetGlobalStats,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  KeyboardLayouts,
  DownloadFonts,
  ClockSync,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  uint16_t CrossPointSettings::* value16Ptr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<uint8_t> enumRawValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint16_t min;
    uint16_t max;
    uint16_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  SettingInfo& withEnumRawValues(std::vector<uint8_t> values) {
    enumRawValues = std::move(values);
    return *this;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Submenu(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SUBMENU;
    s.action = action;
    return s;
  }

  static SettingInfo SectionHeader(StrId nameId) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::SECTION_HEADER;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Value16(StrId nameId, uint16_t CrossPointSettings::* ptr, const ValueRange valueRange,
                             const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.value16Ptr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

inline size_t settingEnumOptionCount(const SettingInfo& setting) {
  return setting.enumStringValues.empty() ? setting.enumValues.size() : setting.enumStringValues.size();
}

inline std::string settingEnumOptionLabel(const SettingInfo& setting, const uint8_t displayIndex) {
  if (!setting.enumStringValues.empty()) {
    return displayIndex < setting.enumStringValues.size() ? setting.enumStringValues[displayIndex] : std::string();
  }
  return displayIndex < setting.enumValues.size() ? std::string(I18N.get(setting.enumValues[displayIndex]))
                                                  : std::string();
}

inline bool settingShowsNavigationCaret(const SettingInfo& setting) {
  return setting.type == SettingType::SUBMENU || setting.action == SettingAction::CustomiseStatusBar ||
         setting.action == SettingAction::QuickActions;
}

class SettingsActivity final : public Activity {
 public:
  enum class View : uint8_t { Root, FileBrowser };

 private:
  ButtonNavigator buttonNavigator;

  int selectedCategoryIndex = 0;  // Currently selected category
  int selectedSettingIndex = 0;
  int settingsCount = 0;

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> displaySleepSettings;
  std::vector<SettingInfo> displayFrontlightSettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> readerFontSettings;
  std::vector<SettingInfo> readerPageLayoutSettings;
  std::vector<SettingInfo> readerScreenMarginSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> controlsPowerSettings;
  std::vector<SettingInfo> controlsHomeButtonSettings;
  std::vector<SettingInfo> controlsFrontButtonSettings;
  std::vector<SettingInfo> controlsSideButtonSettings;
  std::vector<SettingInfo> controlsTapsGesturesSettings;
  std::vector<SettingInfo> controlsTwoFingerSwipeSettings;
  std::vector<SettingInfo> systemSettings;
  std::vector<SettingInfo> systemDeviceSettings;
  std::vector<SettingInfo> systemFilesCacheSettings;
  std::vector<SettingInfo> fileBrowserSettings;
  std::vector<SettingInfo> systemReadingStatsSettings;
  std::vector<SettingInfo> systemGlobalStatsSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;
  // The frontlight-panel shortcut opens Settings as a transient Home menu.
  // Its swipe-up closes the screen; regular Settings keeps swipe scrolling.
  bool dismissOnUpSwipe = false;
  // The frontlight drawer pushes Settings over an active reader. Closing that
  // instance must pop back to the panel instead of replacing the reader with Home.
  bool returnToParentOnClose = false;
  View view = View::Root;
  // Settings can be created over a landscape reader. Its teardown resets the
  // renderer before this activity enters, so retain the requested layout.
  GfxRenderer::Orientation entryOrientation;
  bool showSettingSelection = true;
  SettingAction activeSubmenu = SettingAction::None;
  SettingAction parentSubmenu = SettingAction::None;

  OptionPopup optionPopup;

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  // FreeInkApp hosts the tab bar + settings list (themed, touch-routed); the
  // header stays on GUI.drawHeader for the battery, OptionPopup stays legacy.
  using UiApp = freeink::ui::FreeInkApp<24, 4>;
  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void settingsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static std::string settingValueText(const SettingInfo& setting);
  void buildSettingsScreen(UiApp::ScreenType& screen);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void enterCategory(int categoryIndex);
  void setCurrentSettingsForCategory();
  StrId activeSubmenuTitleId() const;
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  bool currentSettingUsesOptionMenu(const SettingInfo& setting) const;
  void openEnumOptionPicker(const SettingInfo& setting);
  void openScreenMarginPicker(const SettingInfo& setting);
  void openWordSpacingPicker();
  void openLanguagePicker();
  void openIdleTimeThresholdPicker();
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void openLineHeightPicker();
  void openFrontlightScheduleTimePicker(uint16_t CrossPointSettings::* valuePtr, StrId titleId);
  void openStringEditor(const SettingInfo& setting);
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  void closeRootSettings();
  bool isFileBrowserView() const { return view == View::FileBrowser; }

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool dismissOnUpSwipe = false,
                            bool returnToParentOnClose = false, View view = View::Root);
  bool allowGlobalHomeSwipeGesture() const override { return false; }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
