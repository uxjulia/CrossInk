#pragma once
#include <Txt.h>

#include <optional>
#include <vector>

#include "BookLoadingFeedback.h"
#include "CrossPointSettings.h"
#include "PluginReaderEvents.h"
#include "ReaderButtonShortcuts.h"
#include "ReaderProgressSaveDebouncer.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#if CROSSINK_APP_CAP_TOUCH
#include "activities/reader/ReaderPinchGesture.h"
#endif

class TxtReaderActivity final : public Activity {
  PluginReaderEvents pluginEvents;
#ifdef SIMULATOR
  friend struct ReaderMenuOrientationSmokeTest;
  friend struct StatusBarFeatureSmokeTest;
  friend struct ScreenCalibrationSmokeTest;
#endif
  OptionPopup quickActionsPopup;
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  // Session-only display toggle; cached page layout remains unchanged.
  bool statusBarVisible = true;
  ReaderButtonShortcuts readerButtonShortcuts;
  bool longPowerButtonHandled = false;
  bool longPressBackHandled = false;
  bool longPressMenuHandled = false;
  bool skipRecentBookUpdateOnEntry = false;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
#if CROSSINK_APP_CAP_TOUCH
  ReaderPinchGesture pinchFontGesture;
#endif

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int8_t cachedCharacterSpacing = 0;
  int cachedFontId = 0;
  uint8_t cachedVerticalMargin = 0;
  uint8_t cachedHorizontalMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedTopStatusBarHeight = 0;
  int cachedFooterReservedHeight = 0;
  ScreenInsets cachedViewableInsets;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;
  // Keep one content anchor through repeated bar edits until the reader turns a page.
  std::optional<size_t> statusBarRelayoutOffset;
  int statusBarRelayoutPage = -1;

  void renderPage();
  void renderStatusBar() const;
  void requestPageTurn(bool previous, bool next);

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        uint8_t* scratch = nullptr);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  bool saveProgress(int page);
  bool queueProgressSave();
  bool flushQueuedProgress();
  void saveProgressBeforeRestart();
  void loadProgress();
  void toggleDarkMode();
  void toggleHomeButtonInReader();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  static bool supportsQuickAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeReaderShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executePowerButtonAction();
  bool executeLongPressBackAction();
  bool changeReaderFontSize(bool larger, FontSizeStepMode mode = FontSizeStepMode::Wrap);
  void cycleReaderFont();
  // Font changes and layout invalidation share the caller's RenderLock.
  void rebuildTextLayout();
  bool applyReaderOrientation(uint8_t orientation);
#if CROSSINK_APP_CAP_TOUCH
  bool handlePinchFontResize();
  void resetPinchFontGesture();
#endif

  BookLoadingFeedback openingFeedback;
  bool showOpeningFeedback() {
    if (!openingFeedback.showIfPending(renderer)) return false;
    pagesUntilFullRefresh = 0;
    return true;
  }

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown, bool skipRecentBookUpdateOnEntry = false,
                             BookLoadingFeedback openingFeedback = {})
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry),
        openingFeedback(openingFeedback) {}
  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override {
    if (!flushQueuedProgress()) LOG_ERR("PEVT", "Reader progress save failed before sleep");
    pluginEvents.poll(getCurrentBookPath());
    pluginEvents.flush(getCurrentBookPath());
  }
  void loop() override;
  void render(RenderLock&&) override;
  bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) override;
  bool handleTwoFingerRotation(bool clockwise) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    return true;
  }
  bool isReaderActivity() const override { return true; }
  bool isBookReaderActivity() const override { return true; }
  bool usesMenuButtonOrientation() const override { return quickActionsPopup.isActive(); }
  bool usesFullScreenReaderVerticalSwipes() const override {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
    return true;
#else
    return false;
#endif
  }
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(uint8_t action) override;
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  std::string getCurrentBookPath() const override { return txt ? txt->getPath() : std::string{}; }
  std::string getCurrentBookTitle() const override { return txt ? txt->getTitle() : std::string{}; }
  bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) override;
  void onFrontlightPanelOpened() override { saveProgressBeforeRestart(); }
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
