#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "ReaderProgressSaveDebouncer.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#if CROSSINK_APP_CAP_TOUCH
#include "activities/reader/ReaderPinchGesture.h"
#include "activities/reader/TouchReaderPreviewModel.h"
#endif

struct ToastRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

class EpubReaderActivity final : public Activity {
 public:
  bool usesFullScreenReaderVerticalSwipes() const override { return true; }

  struct ReaderSettingsSnapshot {
    uint8_t fontFamily = 0;
    uint8_t readerFontPointSize = 14;
    uint8_t lineHeightPercent = 100;
    uint8_t wordSpacing = 0;
    uint8_t orientation = 0;
    uint8_t screenMarginVertical = 5;
    uint8_t screenMarginHorizontal = 5;
    uint8_t publisherPageNumbers = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 1;
    uint8_t hyphenationEnabled = 0;
    uint8_t textAntiAliasing = 1;
    uint8_t imageRendering = 0;
    uint8_t extraParagraphSpacing = 1;
    uint8_t forceParagraphIndents = 0;
    uint8_t bionicReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    uint8_t epubRenderMode = 0;
    uint8_t indexingMethod = CrossPointSettings::INDEXING_FULL_SECTION;
    char sdFontFamilyName[64] = "";
  };

  struct BookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    bool hasRenderModeOverride = false;
    bool hasDictionaryFontOverride = false;
    uint8_t renderMode = 0;
    // Fixed-size per-book state lives inside the already heap-owned reader
    // activity. It avoids a separate string allocation during dictionary use.
    char dictionarySdFontFamilyName[64] = "";
    // Zero follows the reader's current physical point size.
    uint8_t dictionaryFontPointSize = 0;
    ReaderSettingsSnapshot readerSettings;
  };

 private:
  // The on-disk settings record also carries the dictionary family. Keeping it
  // out of the long-lived reader object matters on the C3: this activity is
  // allocated immediately before an EPUB section needs its largest block.
  // Dictionary children load and own the fixed name only while they are open.
  struct ActiveBookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    bool hasRenderModeOverride = false;
    uint8_t renderMode = 0;
    ReaderSettingsSnapshot readerSettings;

    ActiveBookReaderSettingsData() = default;
    explicit ActiveBookReaderSettingsData(const BookReaderSettingsData& source)
        : hasAutoPageTurnInterval(source.hasAutoPageTurnInterval),
          autoPageTurnSeconds(source.autoPageTurnSeconds),
          hasCustomReaderSettings(source.hasCustomReaderSettings),
          hasRenderModeOverride(source.hasRenderModeOverride),
          renderMode(source.renderMode),
          readerSettings(source.readerSettings) {}
  };

  std::shared_ptr<Epub> epub;
  ActiveBookReaderSettingsData initialBookReaderSettings;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeSectionFontId = 0;
  uint32_t activeSectionLayoutSignature = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  std::string pendingFootnotePreviewAnchor;
  bool activeFootnotePreview = false;
  int pagesUntilFullRefresh = 0;
  // A Sync Progress return can leave non-reader UI on the panel. This is a
  // one-shot clean base for its first image page; normal image-page cleanup
  // uses pagesUntilFullRefresh independently.
  bool cleanImageBasePending = false;
  bool skipRecentBookUpdateOnEntry = false;
  int cachedSpineIndex = 0;
  int cachedChapterPageNumber = 0;
  int cachedChapterTotalPageCount = 0;
  int cachedChapterPageWatermark = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  struct ChapterGroupEstimateCache {
    int currentSpineIndex = -1;
    int firstSpineIndex = -1;
    int lastSpineIndex = -1;
    uint32_t settingsSignature = 0;
    uint64_t knownSiblingImageUnits = 0;
    uint64_t knownSiblingNonImageUnits = 0;
    uint32_t knownSiblingBytes = 0;
    uint32_t unknownSiblingBytes = 0;
    uint64_t precedingKnownImageUnits = 0;
    uint64_t precedingKnownNonImageUnits = 0;
    uint32_t precedingUnknownBytes = 0;
    uint16_t unknownSiblingCount = 0;
    uint16_t precedingUnknownCount = 0;
    bool siblingEstimateUsed = false;
    bool valid = false;
  } chapterGroupEstimate;
  bool pendingRelayoutReposition = false;
  uint16_t cachedPageParagraphIndex = UINT16_MAX;
  uint16_t cachedPageParagraphOffset = 0;
  uint16_t cachedPageParagraphSpan = 0;
  std::atomic<uint8_t> pendingHeapShapeReaderRedrawStages{0};
  static constexpr uint8_t HEAP_SHAPE_REDRAW_CLIP = 1U << 0;
  static constexpr uint8_t HEAP_SHAPE_REDRAW_DICT = 1U << 1;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  unsigned long pageShownAtMs = 0UL;
  unsigned long lastRenderCompleteMs = 0UL;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  int idlePrewarmFontId = 0;
  bool paceSampleWarmupPending = true;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  uint32_t sessionReadingSeconds = 0;
  uint16_t lastAutoPageTurnIntervalSeconds = 0;
  bool bookHasCustomReaderSettings = false;
  bool bookHasAutoPageTurnInterval = false;
  bool bookHasRenderModeOverride = false;
  bool restoreGlobalReaderSettingsOnExit = false;
  ReaderSettingsSnapshot globalReaderSettingsBeforeBook;
  bool bookReaderSettingsSuspendedForGlobalEdit = false;
  ReaderSettingsSnapshot suspendedBookReaderSettings;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  uint16_t pendingParagraphIndex = UINT16_MAX;
#if CROSSINK_APP_CAP_TOUCH
  ReaderDrawerState touchReaderDrawerState{};
  std::unique_ptr<TouchReaderPreviewModel> touchReaderPreviewModel;
  bool touchReaderPreviewAllocationAttempted = false;
#endif
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  // Session-only display toggle. Layout continues to reserve the same status
  // lane, so switching it never changes the EPUB's page breaks.
  bool statusBarVisible = true;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  OptionPopup quickActionsPopup;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  bool touchDictionaryLookupHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  bool homeButtonInReaderFeedback = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  bool pendingRenderModeToast = false;
  bool renderModeToastShown = false;
  bool pendingSafeModeToast = false;
  bool safeModeToastShown = false;
  uint8_t renderModeToastMode = 0;
  unsigned long renderModeToastShowTime = 0UL;
  std::unique_ptr<uint8_t[]> renderModeToastRegionBuffer;
  size_t renderModeToastRegionBufferSize = 0;
  ToastRect renderModeToastRegion;
  bool renderModeToastRegionSaved = false;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool completionTriggerCrossed = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // The end screen owns these UI resources only while it is visible.
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
#if CROSSINK_APP_CAP_TOUCH
  ReaderPinchGesture pinchFontGesture;
  struct FootnoteTouchTarget {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
  };
  std::array<FootnoteTouchTarget, EPUB_MAX_FOOTNOTES_PER_PAGE> currentPageFootnoteTouchTargets{};
#endif
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with identical layout parameters to the pages already rendered.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() does not retry every tick.
  bool partialRebuildStartFailed = false;
  // Set when a background extension build aborted for low heap. startBuild() still succeeds in
  // that state -- it is the layout inside buildSomeMore() that runs out of memory -- so without
  // this flag loop() would restart the same doomed build every tick, and skipLoopDelay() would
  // hold the loop at full speed while it did. The reader keeps the pages already laid out; a
  // build is only re-attempted from render() if the reader actually pages past the watermark.
  bool partialRebuildAbortedForLowMemory = false;
  // One-shot guard for the silent restart used before EPUB layout fallback modes. The restart token
  // restores this guard after boot so the resumed attempt can fall through to those modes.
  bool lowMemoryPartialRestartAttempted = false;
  bool backgroundBuildPausedForLowMemory = false;
  // Input should win the next RenderLock race. Keep the incremental parser alive,
  // but do not start another background chunk until the requested render begins.
  std::atomic<bool> backgroundBuildYieldForInput{false};
  // Full-section next-chapter prefetch is speculative. A forward page turn may
  // stop it, but the visible build at the chapter boundary remains full-section.
  std::atomic<bool> silentPrefetchBuildActive{false};
  std::atomic<bool> silentPrefetchCancelRequested{false};
  std::atomic<bool> sectionBuildCancelRequested{false};
  std::atomic<bool> goHomeAfterBuildCancel{false};

  // Last position successfully persisted by saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders.
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
  bool progressSaveRequiredAfterRelayout = false;
  // Adapted from Sichroteph/YACP commit 3f3c5fc42e794c021edb9832856ef98c2d2065b9
  // (MIT): retain one render-only strip instead of reallocating it on every
  // grayscale page. Internal-heap storage is released before section/index work;
  // PSRAM storage remains reusable for the reader activity lifetime.
  HeapByteBuffer grayscaleStripScratch;
  size_t grayscaleStripScratchSize = 0;
  bool grayscaleStripScratchInPsram = false;
  // Trigger/memoization concept adapted from Sichroteph/YACP commit
  // 3f3c5fc42e794c021edb9832856ef98c2d2065b9 (MIT).
  int preparedNextSpineIndex = -1;
  uint16_t preparedNextViewportWidth = 0;
  uint16_t preparedNextViewportHeight = 0;

  bool renderContents(std::unique_ptr<Page> page, int fontId, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft, bool updatePanel);
  bool ensureGrayscaleStripScratch();
  void releaseGrayscaleStripScratch(bool force = false);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void refreshChapterGroupEstimate(uint16_t viewportWidth, uint16_t viewportHeight);
  bool resolveChapterGroupPageProgress(int& currentPage, int& pageCount, float& chapterProgress,
                                       bool& pageCountEstimated) const;
  bool isTocChapterDestination(int targetSpineIndex, const std::string& anchor) const;
  bool shouldUseFootnotePreview(int targetSpineIndex, const std::string& anchor) const;
  std::string footnotePreviewCacheSuffix(EpubRenderMode renderMode, const std::string& anchor) const;
  void clearFootnotePreviewState();
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool restoreCurrentPageBufferAfterSilentIndex();
  // Larger batches are reserved for non-interactive work such as sleep-page preparation.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  // Interactive builds stop as soon as the requested page is ready and give the
  // main loop a chance to observe input between pages.
  static constexpr int INTERACTIVE_BUILD_PAGES_PER_CHUNK = 1;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does not immediately restart its whole-chapter extension build.
  // Start it only when the reader is close enough to need pages past the watermark.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // If a build predicted to be fast still has not produced the requested page within this
  // window, show the popup while the blocking build continues.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // Only true during the blocking build-to-target phase. The parser retains the callback during
  // background indexing, so this guard prevents it from drawing over an already-visible page.
  bool buildPopupPending = false;
  void showBuildPopup();
  // Remap the cached reading position once the saved paragraph and prior readable watermark are rebuilt
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  bool isRelayoutCatchUpComplete() const;
  bool applyDeferredReposition();
  // Saves are suppressed while a footnote preview is on screen so the preview's own
  // position cannot overwrite the reader's. Set allowDuringFootnotePreview for the
  // deliberate on-exit save of the pre-footnote origin, which is the position the
  // suppression exists to protect.
  bool saveProgress(int spineIndex, int currentPage, int pageCount, bool allowDuringFootnotePreview = false);
  bool queueProgressSave(int spineIndex, int currentPage, int pageCount, bool forceSave = false);
  bool flushQueuedProgress();
  void cacheCurrentSectionPosition();
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getSessionAveragePaceSeconds(uint16_t& avgSeconds) const;
  void recoverStoredPaceFromSession(const char* reason = "unknown");
  bool getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateProgressTimeLeftSeconds(uint32_t& seconds) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len) const;
  void refreshCachedTimeLeftEstimate();
  void applyBookStatsEditsFromDisk();
  void handleBookStatsReturn(bool returnToReaderMenu);
  void resetCurrentBookStatsAfterDelete();
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false, bool returnToReaderMenu = false);
  void startClipSelection(const DictionaryClippingRequest* dictionaryRequest = nullptr,
                          bool ignoreInitialBackRelease = false);
  void resetReadingPaceData();
  void captureGlobalReaderSettings();
  void restoreGlobalReaderSettings();
  void loadBookReaderSettings();
  void saveCurrentBookReaderSettings();
  void saveDictionaryFontForBook(const char* familyName, uint8_t pointSize);
  void saveGlobalSettingsPreservingBookOverrides();
  void beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void saveReaderOptionsForBook(void* ctx);
  static void saveDictionaryFontForBookReader(void* ctx, const char* familyName, uint8_t pointSize);
  static void saveGlobalSettingsForBookReader(void* ctx);
  static void beginGlobalSettingsEditForBookReader(void* ctx);
  static void endGlobalSettingsEditForBookReader(void* ctx);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void prepareCurrentSectionForRelayout();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action,
                                bool dictionaryLookupFramebufferContainsPage = true,
                                QuickLockTrigger quickLockTrigger = QuickLockTrigger::LongMenu);
  void openQuickActionsPopup();
  void executeFootnoteQuickAction(bool suppressInitialPowerRelease = false);
#if CROSSINK_APP_CAP_TOUCH
  bool handlePinchFontResize();
  void resetPinchFontGesture();
  void buildFootnoteTouchTargets(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft);
  bool handleTouchFootnoteLink(int touchX, int touchY);
#endif
  void suppressPowerShortcutRelease();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void handleClippingJump(const ClippingJumpResult& clipping);
  bool handleTouchDictionaryLookup();
  void openWordSelect(bool framebufferContainsPage, int initialTouchX = -1, int initialTouchY = -1,
                      bool autoLookupInitialWord = false);
  std::unique_ptr<Page> reloadDictionaryLookupPage(int pageOffset = 0);
  void renderDictionaryLookupBackground();
  static std::unique_ptr<Page> reloadDictionaryLookupPageCallback(void* context, int pageOffset);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, bool returnToReaderMenu = false,
                           const PendingOverlayResume* replacementResume = nullptr);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  bool shouldQueueCompletionPromptOnChapterExit() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);
  // Shared dismissal rule for the transient bookmark/completed/tilt confirmations.
  bool transientFeedbackDismissed(unsigned long showTimeMs) const;
  void toggleHomeButtonInReader();
  void showRenderModeToast(uint8_t renderMode);
  void showSafeModeToast();
  bool storeRenderModeToastRegion(const char* msg);
  void drawRenderModeToastBuffer(const char* msg);
  bool restoreRenderModeToastRegion();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false, bool preferFootnotePreview = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const BookReaderSettingsData& readerSettings, int initialRefreshCountdown,
                              bool cleanImageBaseOnEntry = false, bool skipRecentBookUpdateOnEntry = false)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        initialBookReaderSettings(readerSettings),
        pagesUntilFullRefresh(initialRefreshCountdown),
        cleanImageBasePending(cleanImageBaseOnEntry),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) override;
  bool handleTwoFingerRotation(bool clockwise) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    cleanImageBasePending = true;
    return true;
  }
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  // Hold the loop hot only while the build has work this loop would do: a kept-alive
  // build sitting outside the lookahead window is dormant, and reporting it here would
  // pin the CPU at full clock (no power saving, yield-only loop) for the whole read.
  // Mirrors the tick condition in loop(): catch-up phase, or watermark inside the window.
  bool sectionBuildWantsTick() const {
    return section && section->isBuilding() &&
           (!section->activeBuildHasCaughtReadablePages() ||
            static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
  }
  bool backgroundSectionBuildHasHeap();
  void idlePrewarmNextPage();
  bool skipLoopDelay() override {
    return sectionBuildWantsTick() && !backgroundBuildPausedForLowMemory &&
           !backgroundBuildYieldForInput.load(std::memory_order_relaxed);
  }
  bool isReaderActivity() const override { return true; }
  bool isEpubReaderActivity() const override { return true; }
  void onInputLockChanged(bool locked) override;
  bool handleQuickLockUnlock(QuickLockTrigger trigger) override;
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  bool openReaderSettingsMenu() override {
    if (!epub) {
      return false;
    }
    openReaderMenu();
    return true;
  }
  bool handleShortcutAction(uint8_t action) override;
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  std::string getCurrentBookTitle() const override { return epub ? epub->getTitle() : std::string{}; }
  bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) override;
  std::unique_ptr<Activity> createFrontlightReadingStatsActivity() override;
  void onFrontlightPanelOpened() override { pauseReadingPaceTimer("frontlight_panel"); }
  void onFrontlightPanelClosed() override;
  void onBackdropRenderedForOverlay() override { pageShownAtMs = 0UL; }
  void persistFrontlightPanelSettings() override { saveGlobalSettingsPreservingBookOverrides(); }
  void onFrontlightGlobalSettingsOpened() override { beginGlobalSettingsEdit(); }
  void onFrontlightGlobalSettingsClosed() override { endGlobalSettingsEdit(); }
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;
  bool handleExternalReaderMenuAction(uint8_t action) override;
  bool restorePendingOverlay(const PendingOverlayResume& resume) override;
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  static BookReaderSettingsData readBookReaderSettings(const Epub& epub);
  static uint8_t loadBookRenderMode(const std::string& filePath);
  static bool saveBookRenderMode(const std::string& filePath, uint8_t renderMode);
  static bool resetBookReaderSettings(const std::string& filePath);
  ScreenshotInfo getScreenshotInfo() const override;
};
