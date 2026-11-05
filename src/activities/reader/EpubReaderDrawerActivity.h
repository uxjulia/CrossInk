#pragma once

#include <AppCapabilities.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "EpubReaderMenuModel.h"
#include "TouchReaderPreviewModel.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#if CROSSINK_SCALABLE_FONTS
#include "TtfRenderProfileStore.h"
#include "components/OptionPopup.h"
#endif

class EpubReaderDrawerActivity final : public Activity {
 public:
#ifdef SIMULATOR
  friend struct SliderOrientationSmokeTest;
#endif
  using SaveSettingsCallback = void (*)(void* ctx);
  using SaveGlobalSettingsCallback = void (*)(void* ctx);
  using GlobalSettingsEditCallback = void (*)(void* ctx);
  using DictionaryFontChangedCallback = void (*)(void* ctx, const char* familyName, uint8_t pointSize);

  explicit EpubReaderDrawerActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
      const EpubReaderPreviewModel* previewModel, float bookProgressPercent, uint32_t chapterPage,
      uint32_t chapterPageCount, bool chapterPageCountEstimated, bool hasFootnotes, bool hasDictionary,
      bool hasBookmarks, bool hasClippings, bool isCurrentPageBookmarked, bool isBookCompleted,
      bool showReadingPaceReset, bool globalStatsEnabled, bool bookStatsEnabled, uint32_t stableCurrentPage,
      uint32_t stablePageCount, uint16_t autoPageTurnIntervalSeconds, bool automaticPageTurnActive,
      SaveSettingsCallback saveReaderSettingsCallback = nullptr, void* saveReaderSettingsContext = nullptr,
      SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr, void* saveGlobalSettingsContext = nullptr,
      GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr,
      void* beginGlobalSettingsEditContext = nullptr,
      GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr, void* endGlobalSettingsEditContext = nullptr,
      const char* dictionaryFontFamilyName = nullptr, uint8_t dictionaryFontPointSize = 0,
      bool hasDictionaryFontOverride = false, DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr,
      void* dictionaryFontChangedContext = nullptr,
      ReaderDrawerState initialState = initialReaderDrawerState(CROSSINK_APP_CAP_TOUCH),
      std::unique_ptr<EpubReaderPreviewModel> ownedPreviewModel = nullptr);

  // Button menu only: shown under the book title. Set before the activity starts.
  void setChapterTitle(std::string title) { chapterTitle = std::move(title); }

#ifdef SIMULATOR
  friend struct ReaderDrawerLayoutSmokeTest;
  freeink::ui::Rect simulatorHandleRect() const { return drawerHandleRect; }
  const ReaderDrawerState& simulatorState() const { return state; }
  bool simulatorFocusedRowVisible() const {
    return buttonFocusActive && state.selectedIndex >= activeTopIndex() &&
           state.selectedIndex < activeTopIndex() + visibleRows;
  }
#endif
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  // A dirty preview can rebuild the page area itself; after a TTF ID change,
  // an unavailable text snapshot is replaced with a safe blank background.
  bool requiresFreshBackdrop() const override {
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    return false;  // The full-screen menu paints every pixel itself.
#else
    return readerDrawerNeedsExternalBackdrop(previewDirty, previewModel && previewModel->valid(),
                                             previewFontMetricsChanged);
#endif
  }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return true; }
  // Route the touch-screen edge swipe through loop() so it can go Home while
  // preserving the capacitive Home key's existing drawer-back behavior.
  bool allowGlobalHomeSwipeGesture() const override { return false; }
  bool handleHomeGesture() override;

 private:
  using RowId = ReaderDrawerCatalogItem;

  using UiApp = freeink::ui::FreeInkApp<48, 11>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_TAB = 2;
  static constexpr freeink::ui::ActionId ACTION_DISMISS = 3;
  static constexpr freeink::ui::ActionId ACTION_BACK = 4;
  static constexpr freeink::ui::ActionId ACTION_SLIDER = 5;
  static constexpr freeink::ui::ActionId ACTION_STEP = 6;
  static constexpr freeink::ui::ActionId ACTION_CONFIRM = 7;
  // 8 and 9 are taken by the dual-slider panes' second control (ACTION_SLIDER + 3,
  // ACTION_STEP + 3); the keypad grid's digit/dot/OK keys share one action.
  static constexpr freeink::ui::ActionId ACTION_KEYPAD_KEY = 10;
  static constexpr freeink::ui::ActionId ACTION_KEYPAD_BACKSPACE = 11;
  static constexpr size_t WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  const EpubReaderPreviewModel* previewModel = nullptr;
  // Button devices own their bounded sample paragraph while the menu is open.
  // Touch devices use the reader's current-page preview instead.
  std::unique_ptr<EpubReaderPreviewModel> ownedPreviewModel;
  bool previewUnavailable = false;
  freeink::ui::Rect samplePreviewBounds{};
  // Centipercent (0-10000, hundredths of a percent) so the keypad can type a decimal
  // destination; 1.00% is 100 here.
  int percent = 0;
  uint32_t stablePage = 0;
  uint32_t stablePageCount = 0;
  // The value each pane opened with (set once in the constructor), restored by
  // resetKeypadEntry() so backspacing everything or leaving and reopening the pane
  // shows the book's actual position again rather than an abandoned typed value.
  const int percentSeed = 0;
  const uint32_t chapterPage = 0;
  const uint32_t chapterPageCount = 0;
  const bool chapterPageCountEstimated = false;
  const uint32_t stablePageSeed = 0;
  std::string chapterTitle;
  // Touch uses the Percent keypad directly. Button devices start on the slider
  // and can open this keypad by holding Confirm, as in the old selector.
  char entryText[8] = {0};
  uint8_t entryLen = 0;
  uint8_t keypadRow = 0;
  uint8_t keypadCol = 0;
  bool keypadBackspaceFocused = false;
  bool percentKeypadActive = false;
  bool percentConfirmLongPressFired = false;
  int percentBeforeKeypad = 0;
  bool hasFootnotes = false;
  bool hasDictionary = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool globalStatsEnabled = true;
  bool bookStatsEnabled = true;
  bool isCurrentPageBookmarked = false;
  bool isBookCompleted = false;
  bool showReadingPaceReset = false;
  bool settingsChanged = false;
  bool didChangeSettings = false;
  bool previewDirty = false;
  bool previewFontMetricsChanged = false;
  bool fontPreviewLoading = false;
  int16_t previousDrawerEdge = -1;
  bool draggingSlider = false;
  bool sliderTapPending = false;
  bool buttonFocusActive = false;
  ReaderButtonSliderState buttonSliderState{};
  bool automaticPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = READER_AUTO_PAGE_TURN_MIN_SECONDS;

  ReaderDrawerState state{};
  ReaderSettingsDraft draft{};
  const ReaderSettingsDraft sourceSettings;
  ReaderSettingsDraft lastGoodPreviewSettings{};
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
  // Owned here rather than as a render-local array, matching itemWindow/labelWindow
  // above: keeps the render task's stack frame small.
  std::array<freeink::ui::KeyGridKey, 12> keypadKeys{};

  SaveSettingsCallback saveReaderSettingsCallback = nullptr;
  void* saveReaderSettingsContext = nullptr;
  SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  char dictionaryFontFamilyName[64] = {};
  uint8_t dictionaryFontPointSize = 0;
  bool hasDictionaryFontOverride = false;
  DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr;
  void* dictionaryFontChangedContext = nullptr;
  ButtonNavigator buttonNavigator;
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
  static void onKeypadKeyEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onKeypadBackspaceEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildDrawer(UiApp::ScreenType& screen);
  void drawButtonBookHeader();
  bool showsSamplePreview() const;
  bool showsSamplePreviewAaNote(const ReaderSettingsDraft& settings) const;
  bool samplePreviewBesideControls() const;
  void renderPreviewUnavailable();
  void renderSamplePreviewText(const ReaderSettingsDraft& settings, int fontId);
  void buildTabBar(UiApp::ScreenType& screen, freeink::ui::Rect rect, bool drawBottomRule);
  void buildPaneHeader(UiApp::ScreenType& screen);
  void buildRows(UiApp::ScreenType& screen);
  void buildSimplePane(UiApp::ScreenType& screen);
  void buildCharacterSpacingPane(UiApp::ScreenType& screen);
  void buildSpacingPane(UiApp::ScreenType& screen);
  void buildMarginsPane(UiApp::ScreenType& screen);
  void buildPercentPane(UiApp::ScreenType& screen);
  void buildPercentSlider(UiApp::ScreenType& screen);
  void buildStablePagePane(UiApp::ScreenType& screen);
  // Shared by both panes: a readout with a backspace icon, and a 4x3 grid (1-9 / 0,
  // ., OK; Percent only enables "."). There is no separate Confirm button; button
  // devices move focus through the grid and press Confirm on OK.
  void buildDrawerKeypad(UiApp::ScreenType& screen, bool allowDecimal, const char* value);
  void buildAutoPageTurnPane(UiApp::ScreenType& screen);
  void buildConfirmButton(UiApp::ScreenType& screen);
  void buildDictionaryPane(UiApp::ScreenType& screen);
  int currentFontSelectionIndex() const;
  void buildFontFamilyPane(UiApp::ScreenType& screen);
  void buildEnumOptionsPane(UiApp::ScreenType& screen);
  void buildTtfRenderingPane(UiApp::ScreenType& screen);

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
  void completeStablePageSelection();
  void completeAutoPageTurnSelection();
  void notifyDictionaryFontChanged();
  void toggleSetting(RowId row);
  void adjustActiveSlider(int delta);
  void adjustButtonSlider(int direction);
  void setActiveSliderPermille(int16_t permille);
  void appendKeypadDigit(char digit);
  void appendKeypadDecimalPoint();
  void backspaceKeypadEntry();
  void resetKeypadEntry();
  void enterPercentKeypad();
  void exitPercentKeypad();
  void adjustPercentSlider(int steps);
  void moveKeypadFocus(int rowDelta, int colDelta);
  void activateKeypadFocus();
  // Parses entryText (if any digits were typed) into percent/stablePage so the
  // grid's OK key always reads the latest typed value.
  void syncKeypadValue();
  int16_t drawerHeight() const;
  freeink::ui::Rect previewBounds() const;
  bool renderPreview(int& previewFontId, std::optional<FontCacheManager::PrewarmScope>& prewarmScope);
  void renderPreviewWithAntiAliasing(int previewFontId);
  void renderPreviewContents(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void renderPreviewText(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void discoverFonts();
  void refreshTtfRenderingRow();
#if CROSSINK_SCALABLE_FONTS
  OptionPopup ttfResetPopup;
  TtfRenderProfile ttfRenderProfile{};
  TtfRenderProfile initialTtfRenderProfile{};
  bool ttfRenderingChanged = false;

  void rebuildTtfRenderingRows();
  void showTtfRenderingOptions(RowId row);
  bool saveTtfRenderingProfile();
  void finishTtfRenderingEdit();
#endif
  void discoverDictionaries();
  bool saveBookDictionary(const std::string& path);
  const char* rowLabel(RowId row) const;
  const char* rowValue(RowId row, char* buffer, size_t bufferSize) const;
  bool rowIsToggle(RowId row) const;
  bool rowShowsNavigationCaret(RowId row) const;
  bool rowToggleValue(RowId row) const;
  const char* paneTitle() const;
};
