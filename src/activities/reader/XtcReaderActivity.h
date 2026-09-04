/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <memory>
#include <string>
#include <utility>

#include "BookReadingStats.h"
#include "EndOfBookOptions.h"
#include "GlobalReadingStats.h"
#include "ReaderProgressSaveDebouncer.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class XtcReaderActivity final : public Activity {
  OptionPopup quickActionsPopup;
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageShownAtMs = 0UL;
  uint32_t sessionReadingSeconds = 0;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  bool longPowerPageTurnHandled = false;
  // Home-key shortcuts are dispatched before this activity's normal input loop.
  // Queue the turn so it follows the same guarded XTC page-turn path.
  bool shortcutPageTurnPending = false;
  bool shortcutPreviousPagePending = false;
  // Session-only display toggle; fixed-layout XTC pages are never regenerated.
  bool statusBarVisible = true;
  bool longPressMenuHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  bool longPressBackHandled = false;
  bool skipRecentBookUpdateOnEntry = false;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
  // The end screen owns these UI resources only while it is visible.
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage(uint32_t pageToRender);
  void renderStatusBarOverlay(StatusBarOverlayPosition position, uint32_t pageToRender) const;
  StatusBarInfo getStatusBarInfo(uint32_t pageToRender) const;
  bool saveProgress(uint32_t page);
  bool queueProgressSave(uint32_t pageToRender);
  bool flushQueuedProgress();
  void loadProgress();
  void pauseReadingStatsTimer(const char* source = "unknown");
  void resumeReadingStatsTimer(const char* source = "unknown");
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPageTurn(uint32_t seconds, bool recordPace);
  bool formatTimeLeftLabel(char* buf, size_t len, uint32_t pageToRender) const;
  void commitReadingStats();
  void resetCurrentBookStatsAfterDelete();
  void setBookCompleted(bool isCompleted);
  float getCurrentBookProgressPercent() const;
  void openChapterSelection();
  void openReadingStats();
  void deleteBookStats();
  void deleteBookCache();
  void openReaderMenu();
  void onReaderMenuConfirm(int action);
  void toggleHomeButtonInReader();
  static bool supportsQuickAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeReaderShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeLongPressBackAction();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             int initialRefreshCountdown, bool skipRecentBookUpdateOnEntry = false)
      : Activity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    return true;
  }
  bool isReaderActivity() const override { return true; }
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
  void onFrontlightPanelOpened() override { pauseReadingStatsTimer("frontlight_panel"); }
  void onFrontlightPanelClosed() override;
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
