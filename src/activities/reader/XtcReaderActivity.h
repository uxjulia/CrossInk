/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once
#include <Xtc.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "BookLoadingFeedback.h"
#include "BookReadingStats.h"
#include "DailyReadingStats.h"
#include "EndOfBookOptions.h"
#include "GlobalReadingStats.h"
#include "PluginReaderEvents.h"
#include "ReaderButtonShortcuts.h"
#include "ReaderProgressSaveDebouncer.h"
#include "ReadingSessionJournal.h"
#include "ReadingSessionStats.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "sync/SyncPending.h"

class XtcReaderActivity final : public Activity {
  PluginReaderEvents pluginEvents;
#ifdef SIMULATOR
  friend struct ReaderMenuOrientationSmokeTest;
  friend struct StatusBarFeatureSmokeTest;
#endif
  OptionPopup quickActionsPopup;
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageShownAtMs = 0UL;
  uint32_t sessionReadingSeconds = 0;
  DailyReadingStats::Session dailyReadingSession;
  ReadingSessionDates readingSessionDates;
  DeferredReadingStatsInterval deferredReadingStats;
  ReadingStatsSaveDelay statsSaveDelay;
  ReadingSessionJournal::Session readingSessionJournal;
  uint32_t statsCheckpointRevision = 0;
  void startDailyReadingInterval();
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  bool bookStatsEnabled = true;
  bool statsTrackingActive = true;
  bool paceDirty = false;
  bool pendingStatsCommit = false;
  bool longPowerPageTurnHandled = false;
  // Home-key shortcuts are dispatched before this activity's normal input loop.
  // Queue the turn so it follows the same guarded XTC page-turn path.
  bool shortcutPageTurnPending = false;
  bool shortcutPageTurnPendingFromSide = false;
  bool shortcutPreviousPagePending = false;
  bool shortcutPreviousPagePendingFromSide = false;
  // Session-only display toggle; fixed-layout XTC pages are never regenerated.
  bool statusBarVisible = true;
  bool longPressMenuHandled = false;
  ReaderButtonShortcuts readerButtonShortcuts;
  bool longPressBackHandled = false;
  bool skipRecentBookUpdateOnEntry = false;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
  // Records this book's stats on the sync pending-push list once per session.
  SyncPending::SessionMarker pendingSync;
  // The end screen owns these UI resources only while it is visible.
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookMenuActive{false};

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage(uint32_t pageToRender);
  void renderStatusBarOverlay(StatusBarOverlayPosition position, uint32_t pageToRender, bool drawContent,
                              int batteryPercentage) const;
  StatusBarInfo getStatusBarInfo(uint32_t pageToRender, bool includeTitle = false) const;
  bool saveProgress(uint32_t page);
  bool queueProgressSave(uint32_t pageToRender);
  bool flushQueuedProgress();
  // Caller holds RenderLock, including the render-task low-memory path.
  bool checkpointReadingStatsBeforeRestart();
  bool saveProgressBeforeRestart();
  void loadProgress();
  void pauseReadingStatsTimer(const char* source = "unknown");
  void syncStatsTrackingState();
  void resumeReadingStatsTimer(const char* source = "unknown");
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown", bool deferStorage = false);
  void flushDeferredReadingStats(bool allowCheckpoint = true);
  void checkpointReadingStatsWhenIdle();
  void recordForwardPageTurn(uint32_t seconds, bool recordPace);
  bool formatTimeLeftLabel(char* buf, size_t len, uint32_t pageToRender, bool bookEstimate) const;
  void commitReadingStats();
  void finalizeReadingStatsOnExit();
  uint32_t globalStatsResetRevisionAtPanelOpen = 0;
  void applyBookStatsEditsFromDisk();
  void resetCurrentBookStatsAfterDelete();
  void setBookCompleted(bool isCompleted);
  float getCurrentBookProgressPercent() const;
  void openChapterSelection();
  void openPercentSelection();
  void openReadingStats();
  void deleteBookStats();
  void deleteBookCache();
  void openReaderMenu();
  void onReaderMenuConfirm(int action);
  void toggleHomeButtonInReader();
  static bool supportsQuickAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeReaderShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeLongPressBackAction();

  BookLoadingFeedback openingFeedback;
  bool showOpeningFeedback() {
    if (!openingFeedback.showIfPending(renderer)) return false;
    pagesUntilFullRefresh = 0;
    return true;
  }

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             int initialRefreshCountdown, bool skipRecentBookUpdateOnEntry = false,
                             BookLoadingFeedback openingFeedback = {})
      : Activity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
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
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    return true;
  }
  bool isReaderActivity() const override { return true; }
  bool isBookReaderActivity() const override { return true; }
  bool usesMenuButtonOrientation() const override;
  bool usesFullScreenReaderVerticalSwipes() const override {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
    return true;
#else
    return false;
#endif
  }
  void onInputLockChanged(bool locked) override;
  bool handleQuickLockUnlock(QuickLockTrigger trigger) override;
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  bool openReaderSettingsMenu() override {
    if (!xtc) {
      return false;
    }
    openReaderMenu();
    return true;
  }
  std::string getCurrentBookPath() const override { return xtc ? xtc->getPath() : std::string{}; }
  std::string getCurrentBookTitle() const override { return xtc ? xtc->getTitle() : std::string{}; }
  bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) override;
  std::unique_ptr<Activity> createFrontlightReadingStatsActivity() override;
  void onFrontlightPanelOpened() override {
    pauseReadingStatsTimer("frontlight_panel");
    saveProgressBeforeRestart();
    globalStatsResetRevisionAtPanelOpen = GlobalReadingStats::localResetRevision();
  }
  void onFrontlightPanelClosed() override;
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
