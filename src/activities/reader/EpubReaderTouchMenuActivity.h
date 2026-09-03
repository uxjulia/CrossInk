#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_TOUCH

#include <Epub.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpubReaderMenuModel.h"
#include "ReaderOptionsActivity.h"
#include "TouchReaderPreviewModel.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class EpubReaderTouchMenuActivity final : public Activity {
 public:
  explicit EpubReaderTouchMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
      const TouchReaderPreviewModel* previewModel, int bookProgressPercent, bool hasFootnotes, bool hasDictionary,
      bool hasBookmarks, bool hasClippings, bool isCurrentPageBookmarked, bool isBookCompleted,
      bool showReadingPaceReset, bool stablePageNumbersAvailable, uint16_t autoPageTurnIntervalSeconds,
      bool automaticPageTurnActive, ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback,
      void* saveReaderSettingsContext, ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback,
      void* saveGlobalSettingsContext,
      ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback,
      void* beginGlobalSettingsEditContext,
      ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback,
      void* endGlobalSettingsEditContext, const char* dictionaryFontFamilyName, uint8_t dictionaryFontPointSize,
      bool hasDictionaryFontOverride,
      ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback,
      void* dictionaryFontChangedContext, ReaderDrawerState initialState = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  bool requiresFreshBackdrop() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return true; }
  // Route the touch-screen edge swipe through loop() so it can go Home while
  // preserving the capacitive Home key's existing drawer-back behavior.
  bool allowGlobalHomeSwipeGesture() const override { return false; }
  bool handleHomeGesture() override;

 private:
  using RowId = ReaderDrawerCatalogItem;

  using UiApp = freeink::ui::FreeInkApp<48, 9>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_TAB = 2;
  static constexpr freeink::ui::ActionId ACTION_DISMISS = 3;
  static constexpr freeink::ui::ActionId ACTION_BACK = 4;
  static constexpr freeink::ui::ActionId ACTION_SLIDER = 5;
  static constexpr freeink::ui::ActionId ACTION_STEP = 6;
  static constexpr freeink::ui::ActionId ACTION_CONFIRM = 7;
  static constexpr size_t WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  const TouchReaderPreviewModel* previewModel = nullptr;
  int percent = 0;
  bool hasFootnotes = false;
  bool hasDictionary = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool isCurrentPageBookmarked = false;
  bool isBookCompleted = false;
  bool showReadingPaceReset = false;
  bool stablePageNumbersAvailable = false;
  bool settingsChanged = false;
  bool didChangeSettings = false;
  bool previewDirty = false;
  bool previewHasAntiAliasing = false;
  bool draggingSlider = false;
  bool sliderTapPending = false;
  bool buttonFocusActive = false;
  bool automaticPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = READER_AUTO_PAGE_TURN_MIN_SECONDS;

  ReaderDrawerState state{};
  ReaderSettingsDraft draft{};
  ReaderSettingsChangeMask changeMask = ReaderSettingsChangeMask::None;
  std::array<std::vector<RowId>, READER_DRAWER_TAB_COUNT> rootRows;
  std::vector<RowId> paneRows;
  std::vector<std::string> fontLabels;
  std::vector<uint8_t> fontSettingIndexes;
  std::vector<std::string> enumOptionLabels;
  std::vector<uint8_t> enumOptionValues;
  RowId enumOptionRow = RowId::FontSize;
  StrId enumOptionTitle = StrId::STR_NONE_OPT;
  ReaderDrawerPane enumOptionReturnPane = ReaderDrawerPane::Root;
  int16_t enumOptionSelectedIndex = 0;
  int16_t previewedEnumOptionIndex = -1;
  std::vector<std::string> dictionaryLabels;
  std::vector<std::string> dictionaryPaths;
  std::string bookDictionaryPath;
  std::array<std::string, WINDOW_SIZE> labelWindow{};
  std::array<freeink::ui::ListItem, WINDOW_SIZE> itemWindow{};

  ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr;
  void* saveReaderSettingsContext = nullptr;
  ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  char dictionaryFontFamilyName[64] = {};
  uint8_t dictionaryFontPointSize = 0;
  bool hasDictionaryFontOverride = false;
  ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr;
  void* dictionaryFontChangedContext = nullptr;
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  freeink::ui::Rect drawerHandleRect{};

  static void drawerScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDismissEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBackEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onConfirmEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildDrawer(UiApp::ScreenType& screen);
  void buildTabBar(UiApp::ScreenType& screen, freeink::ui::Rect rect, bool drawBottomRule);
  void buildPaneHeader(UiApp::ScreenType& screen);
  void buildRootRows(UiApp::ScreenType& screen);
  void buildSimplePane(UiApp::ScreenType& screen);
  void buildSpacingPane(UiApp::ScreenType& screen);
  void buildMarginsPane(UiApp::ScreenType& screen);
  void buildPercentPane(UiApp::ScreenType& screen);
  void buildAutoPageTurnPane(UiApp::ScreenType& screen);
  void buildConfirmButton(UiApp::ScreenType& screen);
  void buildDictionaryPane(UiApp::ScreenType& screen);
  void buildFontFamilyPane(UiApp::ScreenType& screen);
  void buildEnumOptionsPane(UiApp::ScreenType& screen);

  const std::vector<RowId>& activeRows() const;
  int activeTopIndex() const;
  void activateRow(RowId row);
  void activateListIndex(int index);
  void openPane(ReaderDrawerPane pane);
  void closePane();
  void changeTab(ReaderDrawerTab tab);
  void closeAndReturn(bool cancelled, EpubReaderMenuAction action = EpubReaderMenuAction::GO_HOME,
                      bool reopenDrawer = true);
  void commitSettings();
  static ReaderSettingsDraft captureSettings();
  static void applySettings(const ReaderSettingsDraft& settings);
  void markSettingChanged(ReaderSettingsChangeMask mask);
  void moveSelection(bool forward, bool page);
  void scrollBy(int delta);
  void showEnumOptions(RowId row);
  void openEnumOptions(RowId row, StrId title, std::vector<std::string> labels, std::vector<uint8_t> values,
                       int selectedIndex);
  void selectEnumOption(int index);
  void completePercentSelection();
  void completeAutoPageTurnSelection();
  void notifyDictionaryFontChanged();
  void showDestructiveConfirmation(RowId row, EpubReaderMenuAction action);
  void toggleSetting(RowId row);
  void adjustActiveSlider(int delta);
  void setActiveSliderPermille(int16_t permille);
  bool renderPreview();
  void renderPreviewWithAntiAliasing();
  void renderPreviewContents(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void renderPreviewText(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void discoverFonts();
  void discoverDictionaries();
  bool saveBookDictionary(const std::string& path);
  const char* rowLabel(RowId row) const;
  const char* rowValue(RowId row, char* buffer, size_t bufferSize) const;
  bool rowIsToggle(RowId row) const;
  bool rowShowsNavigationCaret(RowId row) const;
  bool rowToggleValue(RowId row) const;
  const char* paneTitle() const;
};

#endif
