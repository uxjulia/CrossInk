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

#include "BookLoadingFeedback.h"
#include "BookPageEstimate.h"
#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "DailyReadingStats.h"
#include "DeferredGrayscale.h"
#include "EndOfBookOptions.h"
#include "EpubLinkReturnState.h"
#include "EpubReaderMenuModel.h"
#include "FootnoteLinkTargets.h"
#include "GlobalReadingStats.h"
#include "ManualPageTurnQueue.h"
#include "PluginReaderEvents.h"
#include "ReaderButtonShortcuts.h"
#include "ReaderProgressSaveDebouncer.h"
#include "ReadingSessionJournal.h"
#include "ReadingSessionStats.h"
#include "StableReadingPace.h"
#include "activities/Activity.h"
#include "activities/reader/TouchReaderPreviewModel.h"
#include "components/OptionPopup.h"
#include "sync/KeptPosition.h"
#include "sync/SyncPending.h"
#if CROSSINK_APP_CAP_TOUCH
#include "activities/reader/ReaderPinchGesture.h"
#endif

struct ToastRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

class EpubReaderActivity final : public Activity {
  PluginReaderEvents pluginEvents;
#ifdef SIMULATOR
  friend struct BookmarkListSmokeTest;
  friend struct ReaderMenuOrientationSmokeTest;
  friend struct EpubReaderCompletionSmokeTest;
  friend struct ScreenCalibrationSmokeTest;
  friend struct ClippingSelectionSmokeTest;
#endif
 public:
  bool usesFullScreenReaderVerticalSwipes() const override { return true; }

  struct ReaderSettingsSnapshot {
    uint8_t fontFamily = 0;
    uint8_t readerFontPointSize = 14;
    uint8_t lineHeightPercent = 100;
    uint8_t wordSpacing = 0;
    uint8_t characterSpacing = 5;
    uint8_t orientation = 0;
    uint8_t screenMarginVertical = 5;
    uint8_t screenMarginHorizontal = 5;
    uint8_t publisherPageNumbers = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 1;
    uint8_t publisherFonts = 0;
    uint8_t hyphenationEnabled = 0;
    uint8_t textAntiAliasing = 1;
    uint8_t imageRendering = 0;
    uint8_t imageGrayscale = 1;
    uint8_t extraParagraphSpacing = 1;
    uint8_t forceParagraphIndents = 0;
    uint8_t focusReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    uint8_t epubRenderMode = 0;
    uint8_t indexingMethod = CrossPointSettings::INDEXING_FULL_SECTION;
    char sdFontFamilyName[64] = "";
  };

  struct BookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    uint32_t readerSettingsOverrideMask = 0;
    bool hasSafeModeOverride = false;
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
  ReaderUtils::DeferredGrayscalePlanes deferredGrayscalePlanes;
  // The on-disk settings record also carries the dictionary family. Keeping it
  // out of the long-lived reader object matters on the C3: this activity is
  // allocated immediately before an EPUB section needs its largest block.
  // Dictionary children load and own the fixed name only while they are open.
  struct ActiveBookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    uint32_t readerSettingsOverrideMask = 0;
    bool hasSafeModeOverride = false;
    bool hasRenderModeOverride = false;
    uint8_t renderMode = 0;
    ReaderSettingsSnapshot readerSettings;

    ActiveBookReaderSettingsData() = default;
    explicit ActiveBookReaderSettingsData(const BookReaderSettingsData& source)
        : hasAutoPageTurnInterval(source.hasAutoPageTurnInterval),
          autoPageTurnSeconds(source.autoPageTurnSeconds),
          hasCustomReaderSettings(source.hasCustomReaderSettings),
          readerSettingsOverrideMask(source.readerSettingsOverrideMask),
          hasSafeModeOverride(source.hasSafeModeOverride),
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
  int cachedTocIndex = -1;  // resolved against the old layout before a settings-driven reset
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  // Consecutive status-bar edits retain one content anchor until navigation.
  std::optional<uint32_t> statusBarRelayoutOffset;
  int statusBarRelayoutSpine = -1;
  int statusBarRelayoutPage = -1;
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
  ManualPageTurnQueue pendingManualPageTurns;
  QueuedTurnRenderingState queuedTurnRendering;
  bool hyphenationPackChecked = false;
  bool checkHyphenationPack();
  // A position another reader left for this book (sync_parked.bin), offered
  // once the first page is on screen. Allocated only when one is found.
  std::unique_ptr<KeptPosition::Offer> keptOffer;
  bool keptOfferShown = false;
  bool offerKeptPosition();
  void applyKeptPosition(const KeptPosition::Offer& offer);
  unsigned long pageShownAtMs = 0UL;
  unsigned long lastRenderCompleteMs = 0UL;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  int idlePrewarmFontId = 0;
  bool paceSampleWarmupPending = true;
  uint32_t sessionStartPaceMillis = 0;
  uint16_t sessionStartPaceSampleCount = 0;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  BookPageEstimate bookPageEstimate;
  StableReadingPace stableReadingPace;
  // A display fallback while fresh text pace or chapter density is unavailable.
  mutable uint32_t lastBookTimeLeftSeconds = 0;
  double stableTextPosition = 0;
  double stableTextTotal = 0;
  double pendingStableReadStart = 0;
  uint32_t pendingStableReadSeconds = 0;
  int pendingStableReadSpine = -1;
  int pendingStableReadPage = -1;
  bool hasStableTextPosition = false;
  int stableTextSpine = -1;
  int stableTextPage = -1;
  void refreshStableTextProgress();
  uint32_t sessionReadingSeconds = 0;
  DailyReadingStats::Session dailyReadingSession;
  ReadingSessionDates readingSessionDates;
  DeferredReadingStatsInterval deferredReadingStats;
  ReadingStatsSaveDelay statsSaveDelay;
  ReadingSessionJournal::Session readingSessionJournal;
  uint32_t statsCheckpointRevision = 0;
  void startDailyReadingInterval();
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
  bool bookStatsEnabled = true;
  bool statsTrackingActive = true;
  bool paceDirty = false;
  bool pendingStatsCommit = false;
  void syncStatsTrackingState();
  void commitReadingStatsSession();
  void finalizeReadingStatsOnExit();
  uint32_t globalStatsResetRevisionAtPanelOpen = 0;
  uint32_t ttfRenderGenerationAtPanelOpen = 0;
  bool pendingTtfRenderRelayout = false;
  void relayoutAfterTtfRenderChange();
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  std::optional<uint32_t> pendingReferenceUnitOffset;
  uint32_t pendingReferenceUnitCount = 0;
  bool pendingReferenceUnitsAreCharacters = false;
  std::optional<uint16_t> pendingResolvedReferencePage;
  uint16_t pendingParagraphIndex = UINT16_MAX;
  ReaderDrawerState touchReaderDrawerState{};
#if CROSSINK_APP_CAP_TOUCH
  std::unique_ptr<TouchReaderPreviewModel> touchReaderPreviewModel;
  bool touchReaderPreviewAllocationAttempted = false;
#endif
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool automaticPageTurnActive = false;
  // Session-only display toggle. Layout continues to reserve the same status
  // lane, so switching it never changes the EPUB's page breaks.
  bool statusBarVisible = true;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  OptionPopup quickActionsPopup;
  ReaderButtonShortcuts readerButtonShortcuts;
  bool touchDictionaryLookupHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
    Unavailable,
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
  bool pendingFontDamagedToast = false;
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
  std::atomic<bool> endOfBookMenuActive{false};

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
#if CROSSINK_APP_CAP_TOUCH
  ReaderPinchGesture pinchFontGesture;
  FootnoteLinkTargets currentPageFootnoteTouchTargets{};
#endif
  using SavedPosition = EpubLinkReturnState::Position;
  static constexpr int MAX_FOOTNOTE_DEPTH = EpubLinkReturnState::MAX_DEPTH;
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
  // A loan destroys page pixels. The loop normally recomposes under RenderLock;
  // failed storage recovery leaves this set until render() supplies a safe base.
  std::atomic<bool> pageBufferStale{false};
  void invalidatePageBufferAfterBuild(uint32_t loansBefore);
  bool backgroundBuildCanUsePageBuffer() const;
  bool renderQuickActionsPopup();
  void restoreStalePageBufferForInputLock();
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
  // Records this book on the sync pending-push list once per reading session.
  // saveProgress() can run on the render task (low-heap restart), so it only
  // flags the mark; the main loop and onExit() write it.
  SyncPending::SessionMarker pendingSync;
  std::atomic<bool> pendingSyncDue{false};
  void flushPendingSyncMark();
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
  void cancelSilentPrefetchForInput();
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
  // Caller holds RenderLock, including the render-task low-memory path.
  bool checkpointReadingStatsBeforeRestart();
  bool saveProgressBeforeRestart();
  bool saveReadingProgress();
  bool saveLinkStack() const;
  void loadLinkStack();
  void cacheCurrentSectionPosition();
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown", bool deferStorage = false);
  void flushDeferredReadingStats(bool allowCheckpoint = true);
  void checkpointReadingStatsWhenIdle();
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getTimeLeftPaceSeconds(float& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  void refreshBookPageEstimate();
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len, bool bookEstimate) const;
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
  void persistReaderSdFontSettings();
  bool saveGlobalSettingsPreservingBookOverrides();
  bool beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void saveReaderOptionsForBook(void* ctx);
  static void saveDictionaryFontForBookReader(void* ctx, const char* familyName, uint8_t pointSize);
  static void persistReaderSdFontSettingsForBook(void* ctx);
  static void saveGlobalSettingsForBookReader(void* ctx);
  static void beginGlobalSettingsEditForBookReader(void* ctx);
  static void endGlobalSettingsEditForBookReader(void* ctx);
  // Jump to a percentage of the book (0.0-100.0, two decimals meaningful), mapping it to spine and page.
  void jumpToPercent(float percent);
  void jumpToStablePage(uint32_t page);
  void reindexCurrentSection();
  void prepareCurrentSectionForRelayout(bool preserveStatusBarAnchor = false);
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action,
                                bool dictionaryLookupFramebufferContainsPage = true,
                                QuickLockTrigger quickLockTrigger = QuickLockTrigger::LongMenu);
  void openQuickActionsPopup();
  void executeFootnoteQuickAction(bool suppressInitialPowerRelease = false);
  void openFootnoteSelect(bool returnToReaderMenu);
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
  static std::unique_ptr<Page> reloadDictionaryLookupPageCallback(void* context, int pageOffset);
  void onReaderMenuConfirm(EpubReaderMenuAction action, bool returnToReaderMenu = false,
                           const PendingOverlayResume* replacementResume = nullptr);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  bool handleEndOfBookPageTurn(bool isForwardTurn);
  void requestManualPageTurn(bool isForwardTurn, const char* source);
  bool drainPendingManualPageTurn();
  void clearPendingManualPageTurns(bool requestRecoveryRedraw = true);
  void finishManualPageTurnBrakeIfReady();
  void cancelSilentNextChapterPrefetchForForwardTurn();
  bool isAtBookStart() const;
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  float getCurrentBookProgressPercent() const;
  // Caller holds RenderLock. Matches the status bar's chapter-group title.
  std::string currentChapterTitle() const;
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

  BookLoadingFeedback openingFeedback;
  bool showOpeningFeedback() {
    if (!openingFeedback.showIfPending(renderer)) return false;
    pagesUntilFullRefresh = 0;
    return true;
  }

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const BookReaderSettingsData& readerSettings, int initialRefreshCountdown,
                              bool cleanImageBaseOnEntry = false, bool skipRecentBookUpdateOnEntry = false,
                              BookLoadingFeedback openingFeedback = {})
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        initialBookReaderSettings(readerSettings),
        pagesUntilFullRefresh(initialRefreshCountdown),
        cleanImageBasePending(cleanImageBaseOnEntry),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry),
        openingFeedback(openingFeedback) {}
  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override {
    if (!saveReadingProgress()) LOG_ERR("PEVT", "Reader progress save failed before sleep");
    pluginEvents.poll(getCurrentBookPath());
    pluginEvents.flush(getCurrentBookPath());
  }
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
  // Caller must own RenderLock: render() can replace or finalize section.
  bool sectionBuildWantsTick() const {
    return section && section->isBuilding() &&
           (!section->activeBuildHasCaughtReadablePages() ||
            static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
  }
  bool backgroundSectionBuildHasHeap();
  void idlePrewarmNextPage();
  bool skipLoopDelay() override {
    return backgroundBuildCanUsePageBuffer() && sectionBuildWantsTick() && !backgroundBuildPausedForLowMemory &&
           !backgroundBuildYieldForInput.load(std::memory_order_relaxed);
  }
  bool isReaderActivity() const override { return true; }
  bool isBookReaderActivity() const override { return true; }
  bool usesMenuButtonOrientation() const override;
  bool isEpubReaderActivity() const override { return true; }
  void onInputLockChanged(bool locked) override;
  void onUserInput() override;
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
  void onFrontlightPanelOpened() override;
  void onFrontlightPanelClosed() override;
  void onBackdropRenderedForOverlay() override { pageShownAtMs = 0UL; }
  void persistGlobalSettings() override { saveGlobalSettingsPreservingBookOverrides(); }
  bool onFrontlightGlobalSettingsOpened() override { return beginGlobalSettingsEdit(); }
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
  enum class BookSettingsReadStatus : uint8_t { Missing, Loaded, Invalid };
  // Read only this cache's settings record; no EPUB parsing, migration or history scan.
  static BookReaderSettingsData readBookReaderSettingsForSupport(const std::string& cachePath,
                                                                 BookSettingsReadStatus& status);
  static uint8_t loadBookRenderMode(const std::string& filePath);
  static uint8_t loadBookRenderMode(const Epub& epub);
  static bool saveBookRenderMode(const std::string& filePath, uint8_t renderMode);
  static bool resetBookReaderSettings(const std::string& filePath);
  ScreenshotInfo getScreenshotInfo() const override;
};
