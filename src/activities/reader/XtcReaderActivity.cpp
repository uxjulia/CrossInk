/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <optional>

#include "BookStatsActivity.h"
#include "BookStatsTracking.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "QuickActions.h"
#include "ReaderUtils.h"
#include "ReadingSessionStats.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "XtcReaderMenuActivity.h"
#include "XtcReaderNavigation.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
constexpr uint16_t MIN_TIME_LEFT_PACE_SAMPLE_COUNT = 3;
constexpr unsigned long LONG_PRESS_MENU_MS = 600UL;

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, toastBackgroundBlack);
  renderer.drawRect(toastX, toastY, toastW, toastH, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, !toastBackgroundBlack);
  renderer.displayBuffer();
}

enum class XtchRenderPass { Base, Lsb, Msb };

bool streamXtchRenderPass(const Xtc& xtc, const uint32_t pageIndex, const uint16_t pageWidth, const uint16_t pageHeight,
                          GfxRenderer& renderer, const XtchRenderPass pass) {
  const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
  const size_t colBytes = (pageHeight + 7) / 8;
  const xtc::XtcError error =
      xtc.loadPageStreaming(pageIndex, [&](const uint8_t* data, const size_t size, const size_t offset) {
        for (size_t i = 0; i < size; i++) {
          const size_t absoluteOffset = offset + i;
          const bool secondPlane = absoluteOffset >= planeSize;
          const size_t planeOffset = secondPlane ? absoluteOffset - planeSize : absoluteOffset;
          const size_t colIndex = planeOffset / colBytes;
          if (colIndex >= pageWidth) continue;

          const uint16_t x = static_cast<uint16_t>(pageWidth - 1 - colIndex);
          const uint16_t yBase = static_cast<uint16_t>((planeOffset % colBytes) * 8);
          for (uint8_t bit = 0; bit < 8 && yBase + bit < pageHeight; bit++) {
            const uint16_t y = static_cast<uint16_t>(yBase + bit);
            const bool bitSet = ((data[i] >> (7 - bit)) & 1) != 0;
            switch (pass) {
              case XtchRenderPass::Base:
                // Applying both planes onto a white buffer produces bit1 OR bit2.
                if (bitSet) renderer.drawPixel(x, y, true);
                break;
              case XtchRenderPass::Lsb:
                // Starting black, plane 1 clears candidates and plane 2 removes
                // false positives, leaving white only for XTH value 1.
                if (!bitSet) renderer.drawPixel(x, y, secondPlane);
                break;
              case XtchRenderPass::Msb:
                // XTH values 1 and 2 are bit1 XOR bit2. Plane 1 seeds the
                // framebuffer; set plane-2 bits then toggle those pixels.
                if (!secondPlane && bitSet) {
                  renderer.drawPixel(x, y, false);
                } else if (secondPlane && bitSet) {
                  renderer.drawPixel(x, y, !renderer.isPixelBlack(x, y));
                }
                break;
            }
          }
        }
      });
  if (error == xtc::XtcError::OK) return true;
  LOG_ERR("XTR", "Failed to stream XTCH page %lu: %s", pageIndex, xtc::errorToString(error));
  return false;
}
}  // namespace

bool XtcReaderActivity::usesMenuButtonOrientation() const {
  return quickActionsPopup.isActive() ||
         (xtc && currentPage >= xtc->getPageCount() && endOfBookMenuActive.load(std::memory_order_relaxed));
}

void XtcReaderActivity::onEnter() {
  Activity::onEnter();
  showOpeningFeedback();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  // Load saved progress
  loadProgress();

  stats = BookReadingStats::load(xtc->getCachePath());
  globalStats = GlobalReadingStats::load();
  bookStatsEnabled = BookStatsTracking::isBookEnabled(xtc->getCachePath());
  statsTrackingActive = SETTINGS.shouldTrackReadingStats() && bookStatsEnabled;
  paceDirty = false;
  pendingStatsCommit = false;
  sessionReadingSeconds = 0;
  statsCheckpointRevision = 0;
  dailyReadingSession.reset();
  readingSessionDates.reset();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  if (!skipRecentBookUpdateOnEntry) {
    RECENT_BOOKS.addOrUpdateBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  }
  showOpeningFeedback();
  SleepCoverAssets::prepareXtc(*xtc);

  showOpeningFeedback();
  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();

  mappedInput.setReaderMode(false);

  if (!flushQueuedProgress()) {
    LOG_ERR("XTR", "Failed to flush debounced reader progress on exit");
  }
  pluginEvents.exit(getCurrentBookPath(), getScreenshotInfo().progressPercent);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  finalizeReadingStatsOnExit();

  // Generate carousel thumbnails while XTC is still loaded so the home screen
  // can display the cover on the very first render without a loading popup.
  if (xtc &&
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
    xtc->generateThumbBmp(LyraCarouselTheme::kCenterCoverW, LyraCarouselTheme::kCenterCoverH);
    xtc->generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  }

  xtc.reset();
}

void XtcReaderActivity::openReaderMenu() {
  bool hasChapters = false;
  std::string title;
  {
    RenderLock lock(*this);
    if (!xtc) {
      return;
    }
    hasChapters = xtc->hasChapters() && xtc->getChapterCount() > 0;
    title = xtc->getTitle();
  }

  pauseReadingStatsTimer("reader_menu");
  startActivityForResult(
      std::make_unique<XtcReaderMenuActivity>(renderer, mappedInput, std::move(title), hasChapters, stats.isCompleted,
                                              SETTINGS.shouldTrackReadingStats(), bookStatsEnabled),
      [this](const ActivityResult& result) {
        const auto* menu = std::get_if<MenuResult>(&result.data);
        if (result.isCancelled || menu == nullptr) {
          resumeReadingStatsTimer("reader_menu_return");
          requestUpdate();
          return;
        }
        onReaderMenuConfirm(menu->action);
      });
}

void XtcReaderActivity::loop() {
  if (mappedInput.hasPendingActivity() || RenderLock::peek()) statsSaveDelay.defer(millis());
  pluginEvents.pollWhenIdle(getCurrentBookPath());
  if (!xtc) {
    return;
  }
  syncStatsTrackingState();
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool shortcutPageTurn = shortcutPageTurnPending;
  bool shortcutPageTurnFromSide = shortcutPageTurnPendingFromSide;
  shortcutPageTurnPending = false;
  shortcutPageTurnPendingFromSide = false;
  bool shortcutPreviousPage = shortcutPreviousPagePending;
  bool shortcutPreviousPageFromSide = shortcutPreviousPagePendingFromSide;
  shortcutPreviousPagePending = false;
  shortcutPreviousPagePendingFromSide = false;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const int bottomHeight = UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Bottom, renderer);
  const auto topConfig = xtcStatusBarConfigForDisplay(ReaderStatusBarPosition::Top, SETTINGS.legacyXtcTopUsesBottom,
                                                      SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top),
                                                      SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom));
  const int topHeight = UITheme::getReaderStatusBarHeight(
      xtcStatusBarConfigPosition(ReaderStatusBarPosition::Top, SETTINGS.legacyXtcTopUsesBottom), renderer, &topConfig);
  const auto statusBarMode = static_cast<CrossPointSettings::XTC_STATUS_BAR_MODE>(SETTINGS.xtcStatusBarMode);
  const bool tappedStatusBar = touch.tapped && (((statusBarMode == CrossPointSettings::XTC_STATUS_BAR_TOP ||
                                                  statusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTH) &&
                                                 ReaderUtils::isTopStatusBarTap(renderer, touch.y, topHeight)) ||
                                                ((statusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM ||
                                                  statusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTH) &&
                                                 ReaderUtils::isBottomStatusBarTap(renderer, touch.y, bottomHeight)));
  if (tappedStatusBar) {
    if (SETTINGS.tapToHideStatusBar) {
      statusBarVisible = !statusBarVisible;
      requestUpdate();
    }
    return;
  }

  // Paged back into the book: release the end screen app and its theme tokens.
  {
    RenderLock lock(*this);
    if (currentPage < xtc->getPageCount() && endOfBookOptions) {
      endOfBookOptions.reset();
      endOfBookMenuActive.store(false, std::memory_order_relaxed);
    }
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block.
  EndOfBookOptions::Action endOfBookAction = EndOfBookOptions::Action::None;
  std::string openPath;
  bool endOfBookNeedsUpdate = false;
  {
    RenderLock lock(*this);
    if (currentPage >= xtc->getPageCount() && endOfBookOptions && endOfBookOptions->menuActive()) {
      endOfBookAction = endOfBookOptions->handleMenuInput(mappedInput, &openPath);
      if (endOfBookAction == EndOfBookOptions::Action::LastPage) {
        const uint32_t pageCount = xtc->getPageCount();
        currentPage = pageCount > 0 ? pageCount - 1 : 0;
        endOfBookNeedsUpdate = true;
      } else if (endOfBookAction == EndOfBookOptions::Action::Redraw) {
        endOfBookNeedsUpdate = true;
      }
    }
  }
  switch (endOfBookAction) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return;
    case EndOfBookOptions::Action::LastPage:
    case EndOfBookOptions::Action::Redraw:
      if (endOfBookNeedsUpdate) {
        requestUpdate();
      }
      return;
    case EndOfBookOptions::Action::None:
      break;
  }

  if (longPressMenuHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressMenuHandled = false;
    }
    return;
  }

  const auto longPressMenuAction =
      static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction);
  if (longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    longPressMenuHandled = true;
    mappedInput.suppressNextConfirmRelease();
    handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongMenu);
    return;
  }
  if (longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    mappedInput.suppressNextConfirmRelease();
    handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongMenu);
    return;
  }

  if ((longPressMenuAction == CrossPointSettings::LONG_MENU_LIBRARY ||
       ReaderUtils::isNavigationLongPressAction(longPressMenuAction) ||
       longPressMenuAction == CrossPointSettings::LONG_MENU_SELECT_CHAPTER) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS &&
      (mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
       mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    longPressMenuHandled = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    mappedInput.suppressNextConfirmRelease();
    if (longPressMenuAction == CrossPointSettings::LONG_MENU_SELECT_CHAPTER)
      openChapterSelection();
    else if (!ReaderUtils::dispatchNavigationLongPressAction(longPressMenuAction))
      activityManager.goToLibrary();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
    openReaderMenu();
    return;
  }

  if (longPressBackHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackHandled = false;
    }
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    mappedInput.suppressNextBackRelease();
    executeLongPressBackAction();
    return;
  }

  // Short press BACK goes directly to home
  if (!touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto side = readerButtonShortcuts.update(mappedInput, mappedInput.buttonSampleTime());
  if (ReaderButtonShortcuts::shouldConsume(side, mappedInput)) {
    if (side.triggered) {
      if (side.longPress) {
        if (side.front)
          mappedInput.suppressNextFrontRelease(side.up ? MappedInputManager::Button::Left
                                                       : MappedInputManager::Button::Right);
        else
          mappedInput.suppressNextSideRelease(side.up ? MappedInputManager::Button::Up
                                                      : MappedInputManager::Button::Down);
      }
      switch (side.action) {
        case CrossPointSettings::PAGE_TURN:
          shortcutPageTurn = true;
          shortcutPageTurnFromSide = true;
          break;
        case CrossPointSettings::PREVIOUS_PAGE:
          shortcutPreviousPage = true;
          shortcutPreviousPageFromSide = true;
          break;
        case CrossPointSettings::SIDE_PREVIOUS_CHAPTER:
        case CrossPointSettings::SIDE_NEXT_CHAPTER: {
          // Fixed-page books keep the established ten-page chapter-skip step.
          const bool next = side.action == CrossPointSettings::SIDE_NEXT_CHAPTER;
          if (!next && currentPage == 0) break;
          bool goHome = false;
          {
            RenderLock lock(*this);
            const uint32_t pageCount = xtc->getPageCount();
            if (currentPage >= pageCount) {
              if (next)
                goHome = true;
              else
                currentPage = pageCount > 0 ? pageCount - 1 : 0;
            } else {
              uint32_t forwardReadSeconds = 0;
              const bool recordForward = next && forwardPageReadElapsed(forwardReadSeconds, "side_long_press");
              recordCurrentPageReadingTime("side_long_press", /*deferStorage=*/true);
              currentPage =
                  next ? std::min<uint32_t>(currentPage + 10, pageCount) : (currentPage >= 10 ? currentPage - 10 : 0);
              if (recordForward) recordForwardPageTurn(forwardReadSeconds, false);
            }
          }
          if (goHome)
            onGoHome();
          else
            requestUpdate();
          break;
        }
        case CrossPointSettings::SIDE_INCREASE_FONT:
        case CrossPointSettings::SIDE_DECREASE_FONT:
        case CrossPointSettings::SIDE_ROTATE_COUNTERCLOCKWISE:
        case CrossPointSettings::SIDE_ROTATE_CLOCKWISE:
        case CrossPointSettings::SIDE_ROTATE_FLIP:
        case CrossPointSettings::IGNORE:
          // XTC pages are pre-rendered; font size and rotation do not apply.
          break;
        default: {
          const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(side.action);
          if (action == CrossPointSettings::QUICK_LOCK)
            handleGlobalPowerButtonAction(action, ReaderButtonShortcuts::quickLockTrigger(side));
          else if (!handleShortcutAction(action))
            handleGlobalPowerButtonAction(action);
          break;
        }
      }
    }
    if (!shortcutPageTurn && !shortcutPreviousPage) return;
  }

  const auto tilt = detectReaderTiltPageTurn(halTiltSensor, SETTINGS.tiltPageTurn, SETTINGS.tiltNextPageOnly);
  const bool frontPrev = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = mappedInput.wasReleased(MappedInputManager::Button::Power);
  if (longPowerPageTurnHandled && (powerReleased || !mappedInput.isPressed(MappedInputManager::Button::Power))) {
    longPowerPageTurnHandled = false;
    return;
  }
  if (powerReleased && mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration() &&
      executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn))) {
    return;
  }
  if (!longPowerPageTurnHandled && mappedInput.isPressed(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration() &&
      executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn))) {
    // Reader long-press actions execute while Power is still held. Consume its
    // later release so the app-wide shortcut dispatcher cannot run it again.
    mappedInput.suppressNextPowerRelease();
    longPowerPageTurnHandled = true;
    return;
  }

  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool timedLongPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                                  !longPowerPageTurnHandled &&
                                  mappedInput.isPressed(MappedInputManager::Button::Power) &&
                                  mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  if (timedLongPowerTurn) {
    longPowerPageTurnHandled = true;
  }
  const bool powerPageTurn = shortPowerTurn || longPowerTurn || timedLongPowerTurn;
  const bool frontNext = mappedInput.wasReleased(MappedInputManager::Button::Right) || powerPageTurn;

  const bool prevTriggered = tilt.prev || frontPrev || touch.prev || shortcutPreviousPage;
  const bool nextTriggered = tilt.next || frontNext || touch.next || shortcutPageTurn;
  if (!prevTriggered && !nextTriggered) {
    checkpointReadingStatsWhenIdle();
    return;
  }

  // Mirror EPUB's manual-turn guard: the render task updates the panel
  // concurrently, so accepting another turn before it owns RenderLock can mix
  // two pages. The short time gap covers that request-to-render startup window.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  const unsigned long now = millis();
  if (RenderLock::peek() || (now - lastPageTurnTime) < kMinManualTurnGapMs) {
    shortcutPageTurnPending = shortcutPageTurnPending || shortcutPageTurn;
    shortcutPageTurnPendingFromSide = shortcutPageTurnPendingFromSide || shortcutPageTurnFromSide;
    shortcutPreviousPagePending = shortcutPreviousPagePending || shortcutPreviousPage;
    shortcutPreviousPagePendingFromSide = shortcutPreviousPagePendingFromSide || shortcutPreviousPageFromSide;
    return;
  }
  lastPageTurnTime = now;

  constexpr int skipAmount = 1;

  bool goHome = false;
  bool needsUpdate = false;
  {
    RenderLock lock(*this);
    const uint32_t pageCount = xtc->getPageCount();
    // At end of the book with no suggestion menu, forward button goes home and back
    // button returns to last page.
    if (currentPage >= pageCount) {
      if (endOfBookOptions && endOfBookOptions->menuActive()) {
        // Selection movement was handled above; absorb leftover page-turn triggers so
        // e.g. "previous" at the top of the list doesn't jump back into the book.
        return;
      }
      if (nextTriggered) {
        goHome = true;
      } else {
        currentPage = pageCount > 0 ? pageCount - 1 : 0;
        needsUpdate = true;
      }
    } else if (prevTriggered && currentPage > 0) {
      pluginEvents.noteTurn(false);
      recordCurrentPageReadingTime("page_back", /*deferStorage=*/true);
      if (currentPage >= static_cast<uint32_t>(skipAmount)) {
        currentPage -= skipAmount;
      } else {
        currentPage = 0;
      }
      needsUpdate = true;
    } else if (nextTriggered) {
      uint32_t forwardReadSeconds = 0;
      const bool shouldRecordForwardRead = forwardPageReadElapsed(forwardReadSeconds, "page_forward");
      recordCurrentPageReadingTime("page_forward", /*deferStorage=*/true);
      pluginEvents.noteTurn(true);
      currentPage += skipAmount;
      if (currentPage >= pageCount) {
        currentPage = pageCount;  // Allow showing "End of book"
      }
      if (shouldRecordForwardRead) {
        recordForwardPageTurn(forwardReadSeconds, true);
      }
      needsUpdate = true;
    }
  }
  if (goHome) {
    onGoHome();
  } else if (needsUpdate) {
    requestUpdate();
  }
}

bool XtcReaderActivity::handleTwoFingerSwipeAction(const CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) {
  if (action == CrossPointSettings::TWO_FINGER_SWIPE_SELECT_CHAPTER) {
    openChapterSelection();
    return true;
  }
  // XTC pages are pre-rendered images: they cannot be reflowed for font-size
  // changes, and the reader does not expose stable chapter jumps. Consume the
  // configured command without letting it turn into a regular page swipe.
  return true;
}

void XtcReaderActivity::toggleHomeButtonInReader() {
  if (!mappedInput.hasHomeKey()) return;
  SETTINGS.homeButtonInReaderEnabled = SETTINGS.homeButtonInReaderEnabled ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("XTR", "Failed to save Home button reader setting");
  }
  mappedInput.clearDeferredHomeGesture();
  drawToast(renderer, SETTINGS.homeButtonInReaderEnabled ? tr(STR_HOME_BUTTON_ENABLED) : tr(STR_HOME_BUTTON_DISABLED));
  delay(1000);
  requestUpdate();
}

void XtcReaderActivity::pauseReadingStatsTimer(const char* source) {
  recordCurrentPageReadingTime(source);
  pageShownAtMs = 0UL;
}

void XtcReaderActivity::syncStatsTrackingState() {
  if (!xtc) return;
  const bool active = SETTINGS.shouldTrackReadingStats() && bookStatsEnabled;
  if (active == statsTrackingActive) return;
  if (statsTrackingActive) {
    pendingStatsCommit = true;
    commitReadingStats();
  }
  statsTrackingActive = active;
  sessionReadingSeconds = 0;
  statsCheckpointRevision = 0;
  dailyReadingSession.reset();
  readingSessionDates.reset();
  pageShownAtMs = millis();
  startDailyReadingInterval();
}

void XtcReaderActivity::resumeReadingStatsTimer(const char*) {
  if (xtc && currentPage < xtc->getPageCount()) {
    pageShownAtMs = millis();
    startDailyReadingInterval();
  } else {
    pageShownAtMs = 0UL;
  }
}

void XtcReaderActivity::onInputLockChanged(const bool locked) {
  if (locked) {
    pauseReadingStatsTimer("quick_lock");
  } else {
    resumeReadingStatsTimer("quick_lock");
  }
}

bool XtcReaderActivity::handleQuickLockUnlock(const QuickLockTrigger trigger) {
  if (trigger == QuickLockTrigger::LongMenu) {
    if (longPressMenuHandled) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        longPressMenuHandled = false;
      }
      return true;
    }
    if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
        mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
      longPressMenuHandled = true;
      mappedInput.suppressNextConfirmRelease();
      handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongMenu);
      return true;
    }
    return false;
  }

  if (trigger == QuickLockTrigger::LongBack) {
    if (longPressBackHandled) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          !mappedInput.isPressed(MappedInputManager::Button::Back)) {
        longPressBackHandled = false;
      }
      return true;
    }
    if (SETTINGS.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
        mappedInput.isPressed(MappedInputManager::Button::Back) &&
        mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      longPressBackHandled = true;
      mappedInput.suppressNextBackRelease();
      handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongBack);
      return true;
    }
    return false;
  }

  return false;
}

bool XtcReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const {
  seconds = 0;
  if (!statsTrackingActive || pageShownAtMs == 0UL) {
    return false;
  }

  return readingStatsIntervalSeconds(millis(), pageShownAtMs, SETTINGS.getReadingIdleTimeThresholdSeconds(), seconds);
}

bool XtcReaderActivity::forwardPageReadElapsed(uint32_t& seconds, const char*) const {
  seconds = 0;
  if (pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) {
    return false;
  }

  const uint32_t elapsedSeconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  if (elapsedSeconds > SETTINGS.getReadingIdleTimeThresholdSeconds()) {
    return false;
  }

  seconds = elapsedSeconds;
  return true;
}

void XtcReaderActivity::recordCurrentPageReadingTime(const char* source, const bool deferStorage) {
  // Shortcuts can reach this without a lock; rendering/page turns already hold it.
  std::optional<RenderLock> statsLock;
  if (!RenderLock::heldByCurrentTask()) statsLock.emplace(*this);
  // An unrendered turn must never overwrite the preceding accepted interval.
  flushDeferredReadingStats(!deferStorage);
  uint32_t seconds = 0;
  if (currentPageReadingSecondsForStats(seconds, source)) {
    sessionReadingSeconds = sessionReadingSeconds > UINT32_MAX - seconds ? UINT32_MAX : sessionReadingSeconds + seconds;
    ReadingStatsDateTime end;
    getCurrentLocalDailyReadingDateTime(end);
    readingSessionDates.accept(end, SETTINGS.clockUtcOffsetQ, seconds);
    deferredReadingStats = {end, seconds, SETTINGS.clockUtcOffsetQ};
    if (!deferStorage) flushDeferredReadingStats();
  }
  pageShownAtMs = 0UL;
}

void XtcReaderActivity::checkpointReadingStatsWhenIdle() {
  const uint32_t now = millis();
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || mappedInput.hasPendingActivity() || shortcutPageTurnPending || shortcutPreviousPagePending ||
      deferredReadingStats.seconds != 0) {
    statsSaveDelay.defer(now);
    return;
  }
  if (!statsSaveDelay.ready(now) || (pageShownAtMs != 0 && uint32_t(now - pageShownAtMs) < 1000)) return;
  statsSaveDelay.defer(now);  // Failed writes retry at most once per quiet second.
  if (!readingSessionJournal.checkpointIfDue()) LOG_ERR("SessionStats", "Reading session checkpoint deferred");
  // A captured press during the journal write gets priority over the next file.
  if (mappedInput.hasPendingActivity()) {
    statsSaveDelay.defer(millis());
    return;
  }
  if (xtc && readingSessionJournal.revision() != statsCheckpointRevision &&
      saveReadingSessionCheckpoint(stats, globalStats, sessionReadingSeconds, &readingSessionDates,
                                   xtc->getCachePath())) {
    statsCheckpointRevision = readingSessionJournal.revision();
  }
  if (!DailyReadingStats::checkpointIfDue()) LOG_ERR("DailyStats", "Reading counters checkpoint deferred");
}

void XtcReaderActivity::flushDeferredReadingStats(const bool allowCheckpoint) {
  if (deferredReadingStats.seconds == 0) return;
  const auto interval = deferredReadingStats;
  deferredReadingStats = {};
  const unsigned long startedAt = millis();
  if (xtc && !readingSessionJournal.accept(xtc->getPath().c_str(), interval.end, interval.utcOffset, interval.seconds,
                                           allowCheckpoint)) {
    LOG_ERR("SessionStats", "Reading session checkpoint deferred");
  }
  if (allowCheckpoint && xtc && readingSessionJournal.revision() != statsCheckpointRevision &&
      saveReadingSessionCheckpoint(stats, globalStats, sessionReadingSeconds, &readingSessionDates,
                                   xtc->getCachePath())) {
    statsCheckpointRevision = readingSessionJournal.revision();
  }
  if (!dailyReadingSession.accept(interval.end, interval.utcOffset, interval.seconds, sessionReadingSeconds,
                                  allowCheckpoint)) {
    LOG_ERR("DailyStats", "Cannot record accepted reading interval");
  }
  const unsigned long duration = millis() - startedAt;
  if (duration >= 20) LOG_DBG("SessionStats", "Reading stats storage took %lu ms", duration);
}

void XtcReaderActivity::recordForwardPageTurn(const uint32_t seconds, const bool recordPace) {
  if (recordPace) {
    stats.recordForwardPageRead(seconds);
    paceDirty = true;
  }
  if (statsTrackingActive) {
    stats.totalPagesTurned++;
    globalStats.totalPagesTurned++;
  }
}

bool XtcReaderActivity::formatTimeLeftLabel(char* buf, const size_t len, const uint32_t pageToRender,
                                            const bool bookEstimate) const {
  if (!buf || len == 0 || !xtc) {
    return false;
  }
  const auto pageInfo = getStatusBarInfo(pageToRender);
  const uint32_t current = bookEstimate ? pageToRender + 1U : static_cast<uint32_t>(pageInfo.currentPage);
  const uint32_t total = bookEstimate ? xtc->getPageCount() : static_cast<uint32_t>(pageInfo.pageCount);
  if (current >= total) {
    return false;
  }

  if (stats.avgSecondsPerForwardPage == 0 || stats.paceSampleCount < MIN_TIME_LEFT_PACE_SAMPLE_COUNT) {
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  const uint64_t estimatedSeconds =
      (static_cast<uint64_t>(total - current) * stats.averageForwardPageMillis() + 500U) / 1000U;
  formatCompactReadingDuration(static_cast<uint32_t>(std::min<uint64_t>(estimatedSeconds, UINT32_MAX)), buf, len);
  return true;
}

void XtcReaderActivity::commitReadingStats() {
  if (!xtc) {
    return;
  }
  if (statsTrackingActive) {
    recordCurrentPageReadingTime("session_commit");
    readingSessionJournal.flush();
    commitReadingSession(stats, globalStats, sessionReadingSeconds, &readingSessionDates);
  }
  sessionReadingSeconds = 0;
  readingSessionDates.reset();
  statsCheckpointRevision = 0;
  if ((statsTrackingActive || paceDirty || pendingStatsCommit) && stats.save(xtc->getCachePath())) {
    pendingSync.mark(xtc->getPath(), SyncPending::STATS_BIT);
    paceDirty = false;
    if (statsTrackingActive || pendingStatsCommit) {
      if (globalStats.save()) pendingStatsCommit = false;
    }
  }
}

void XtcReaderActivity::finalizeReadingStatsOnExit() {
  syncStatsTrackingState();
  commitReadingStats();
}

// The stats screen saves a preview that includes the pending session, so keep
// live counters in memory and import only edits.
void XtcReaderActivity::applyBookStatsEditsFromDisk() {
  if (!xtc) return;
  importBookStatsEdits(stats, BookReadingStats::load(xtc->getCachePath()));
  globalStats.completedBooks = GlobalReadingStats::load().completedBooks;
}

void XtcReaderActivity::resetCurrentBookStatsAfterDelete() {
  readingSessionJournal.flush();
  stats = BookReadingStats{};
  sessionReadingSeconds = 0;
  statsCheckpointRevision = 0;
  dailyReadingSession.reset();
  readingSessionDates.reset();
}

void XtcReaderActivity::setBookCompleted(const bool isCompleted) {
  if (!xtc || stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted && statsTrackingActive && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  const bool saved = stats.save(xtc->getCachePath());
  if (saved) pendingSync.mark(xtc->getPath(), SyncPending::STATS_BIT);
  if (saved && statsTrackingActive) {
    if (isCompleted) {
      globalStats.completedBooks++;
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }
    globalStats.save();
  }
}

float XtcReaderActivity::getCurrentBookProgressPercent() const {
  if (!xtc || xtc->getPageCount() == 0) {
    return -1.0f;
  }
  const uint32_t pageCount = xtc->getPageCount();
  const uint32_t clampedPage = currentPage >= pageCount ? pageCount - 1 : currentPage;
  return static_cast<float>(xtc->calculateProgress(clampedPage));
}

bool XtcReaderActivity::getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) {
  RenderLock lock(*this);
  if (!xtc || xtc->getPageCount() == 0) return false;

  const uint32_t pageCount = xtc->getPageCount();
  const uint32_t page = std::min(currentPage, pageCount - 1);
  details.title = xtc->getTitle();
  details.author = xtc->getAuthor();
  details.chapter.clear();
  xtc::ChapterInfo chapter{};
  if (xtc->getChapterForPage(page, chapter)) details.chapter = chapter.name;
  details.progressPercent = xtc->calculateProgress(page);
  return true;
}

std::unique_ptr<Activity> XtcReaderActivity::createFrontlightReadingStatsActivity() {
  if (!xtc || !statsTrackingActive) return {};

  BookReadingStats displayStats = stats;
  displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - sessionReadingSeconds
                                         ? UINT32_MAX
                                         : displayStats.totalReadingSeconds + sessionReadingSeconds;
  uint32_t currentPageSeconds = 0;
  if (currentPageReadingSecondsForStats(currentPageSeconds, "frontlight_stats_preview")) {
    displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - currentPageSeconds
                                           ? UINT32_MAX
                                           : displayStats.totalReadingSeconds + currentPageSeconds;
  }

  const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
  auto statsActivity =
      hasSyncedStats
          ? makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, xtc->getTitle(), xtc->getCachePath(),
                                                 displayStats, getCurrentBookProgressPercent(), false, 0, globalStats,
                                                 GlobalReadingStats::loadAggregated(globalStats))
          : makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, xtc->getTitle(), xtc->getCachePath(),
                                                 displayStats, getCurrentBookProgressPercent(), false, 0, globalStats);
  if (statsActivity) statsActivity->setSyncBookPath(xtc->getPath());
  return statsActivity;
}

void XtcReaderActivity::onFrontlightPanelClosed() {
  // Keep live counters: the drawer stats screen may have saved a preview that
  // already includes the pending session.
  if (xtc) importBookStatsEdits(stats, BookReadingStats::load(xtc->getCachePath()));
  refreshGlobalStatsAfterOverlay(globalStats, globalStatsResetRevisionAtPanelOpen);
  resumeReadingStatsTimer("frontlight_panel_return");
  requestUpdate();
}

void XtcReaderActivity::openChapterSelection() {
  // Direct shortcuts have not passed through the already-paused reader menu.
  // Record their current interval before any success or failure path restarts it.
  pauseReadingStatsTimer("chapter_selection");
  uint32_t pageToSelect = 0;
  bool hasChapters = false;
  {
    RenderLock lock(*this);
    if (xtc) {
      hasChapters = xtc->hasChapters() && xtc->getChapterCount() > 0;
      pageToSelect = currentPage;
    }
  }
  if (!hasChapters) {
    {
      RenderLock lock(*this);
      drawToast(renderer, tr(STR_NO_CHAPTERS));
    }
    delay(1000);
    resumeReadingStatsTimer("chapter_selection_unavailable");
    requestUpdate();
    return;
  }

  auto chapterSelection =
      makeUniqueNoThrow<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, pageToSelect);
  if (!chapterSelection) {
    LOG_ERR("XTR", "OOM: chapter selection activity");
    resumeReadingStatsTimer("chapter_selection_oom");
    requestUpdate();
    return;
  }
  mappedInput.suppressNextConfirmRelease();
  mappedInput.suppressNextPowerRelease();
  mappedInput.suppressNextPowerConfirmRelease();
  startActivityForResult(std::move(chapterSelection), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      RenderLock lock(*this);
      currentPage = std::get<PageResult>(result.data).page;
    }
    resumeReadingStatsTimer("chapter_selection_return");
    requestUpdate();
  });
}

void XtcReaderActivity::openPercentSelection() {
  pauseReadingStatsTimer("percent_selection");
  float progress = 0.0f;
  {
    RenderLock lock(*this);
    if (xtc) progress = getCurrentBookProgressPercent();
  }
  auto selection = makeUniqueNoThrow<EpubReaderPercentSelectionActivity>(renderer, mappedInput, progress);
  if (!selection) {
    LOG_ERR("XTR", "OOM: percent selection activity");
    resumeReadingStatsTimer("percent_selection_oom");
    requestUpdate();
    return;
  }
  mappedInput.suppressNextConfirmRelease();
  mappedInput.suppressNextPowerRelease();
  mappedInput.suppressNextPowerConfirmRelease();
  startActivityForResult(std::move(selection), [this](const ActivityResult& result) {
    const auto* percent = std::get_if<PercentResult>(&result.data);
    if (!result.isCancelled && percent) {
      RenderLock lock(*this);
      if (xtc && xtc->getPageCount() > 0) {
        currentPage = xtcPageForPercent(percent->percent, xtc->getPageCount());
      }
    }
    resumeReadingStatsTimer("percent_selection_return");
    requestUpdate();
  });
}

void XtcReaderActivity::openReadingStats() {
  if (!statsTrackingActive) return;
  auto bookStats = createFrontlightReadingStatsActivity();
  if (!bookStats) {
    resumeReadingStatsTimer("book_stats_no_book");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(bookStats), [this](const ActivityResult&) {
    applyBookStatsEditsFromDisk();
    resumeReadingStatsTimer("book_stats_return");
    requestUpdate();
  });
}

void XtcReaderActivity::deleteBookStats() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                             xtc ? xtc->getTitle() : std::string{}),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && xtc) {
          bool statsDeleted = false;
          {
            RenderLock lock(*this);
            statsDeleted = BookReadingStats::remove(xtc->getCachePath());
            if (statsDeleted) {
              resetCurrentBookStatsAfterDelete();
            }
          }
          if (statsDeleted) {
            drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
            delay(1000);
          } else {
            LOG_ERR("XTR", "Failed to delete book stats");
          }
        }
        resumeReadingStatsTimer("delete_stats_return");
        requestUpdate();
      });
}

void XtcReaderActivity::deleteBookCache() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, confirmationHeading(StrId::STR_DELETE_CACHE),
                                             xtc ? xtc->getTitle() : std::string{}, false, true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && xtc) {
          bool cacheDeleted = false;
          {
            RenderLock lock(*this);
            stats.save(xtc->getCachePath());
            cacheDeleted = clearBookCachePreservingUserState(xtc->getPath());
            xtc->setupCacheDir();
            stats.save(xtc->getCachePath());
          }
          if (cacheDeleted) {
            drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
            delay(1000);
          } else {
            LOG_ERR("XTR", "Failed to delete book cache");
          }
        }
        resumeReadingStatsTimer("delete_cache_return");
        requestUpdate();
      });
}

void XtcReaderActivity::onReaderMenuConfirm(const int action) {
  switch (static_cast<XtcReaderMenuActivity::MenuAction>(action)) {
    case XtcReaderMenuActivity::MenuAction::SELECT_CHAPTER:
      openChapterSelection();
      break;
    case XtcReaderMenuActivity::MenuAction::GO_TO_PERCENT:
      openPercentSelection();
      break;
    case XtcReaderMenuActivity::MenuAction::READING_STATS:
      openReadingStats();
      break;
    case XtcReaderMenuActivity::MenuAction::TOGGLE_BOOK_STATS_TRACKING:
      if (xtc && SETTINGS.shouldTrackReadingStats()) {
        const bool enabled = !bookStatsEnabled;
        if (!BookStatsTracking::setBookEnabled(xtc->getCachePath(), enabled)) {
          drawToast(renderer, (std::string(tr(STR_TRACK_READING_STATS)) + " " + tr(STR_FAILED_LOWER)).c_str());
          delay(1000);
        }
        bookStatsEnabled = BookStatsTracking::isBookEnabled(xtc->getCachePath());
        syncStatsTrackingState();
      }
      resumeReadingStatsTimer("toggle_book_stats_return");
      requestUpdate();
      break;
    case XtcReaderMenuActivity::MenuAction::TOGGLE_COMPLETED:
      setBookCompleted(!stats.isCompleted);
      resumeReadingStatsTimer("toggle_completed_return");
      requestUpdate();
      break;
    case XtcReaderMenuActivity::MenuAction::DELETE_STATS:
      if (statsTrackingActive) deleteBookStats();
      break;
    case XtcReaderMenuActivity::MenuAction::DELETE_CACHE:
      deleteBookCache();
      break;
    case XtcReaderMenuActivity::MenuAction::SEND_NEARBY_BOOK:
      saveProgress(currentPage);
      activityManager.goToNearbyBookSend(xtc ? xtc->getPath() : std::string{}, true);
      return;
    case XtcReaderMenuActivity::MenuAction::DISABLE_TOUCHSCREEN:
      break;
  }
}

bool XtcReaderActivity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (result.action != FrontlightPanelAction::SendNearbyBook || !xtc) return false;
  saveProgress(currentPage);
  return activityManager.goToNearbyBookSend(xtc->getPath(), true);
}

bool XtcReaderActivity::supportsQuickAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SELECT_CHAPTER:
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
    case CrossPointSettings::SHORT_PWRBTN::BACK_HOME:
    case CrossPointSettings::SHORT_PWRBTN::HOME_READER:
      return true;
    default:
      return false;
  }
}

bool XtcReaderActivity::executeReaderShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
      shortcutPageTurnPending = true;
      return true;
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
      shortcutPreviousPagePending = true;
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SELECT_CHAPTER:
      openChapterSelection();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      activityManager.goToFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToCalibreWireless(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToJoinNetworkFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToHotspotFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

bool XtcReaderActivity::executeLongPressBackAction() {
  if (ReaderUtils::dispatchNavigationLongPressAction(SETTINGS.longPressBackAction)) return true;
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction)) {
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_REFRESH_SCREEN:
      prepareManualRefresh();
      requestUpdate();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_TRANSFER:
      activityManager.goToFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CALIBRE_WIRELESS:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToCalibreWireless(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_JOIN_NETWORK:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToJoinNetworkFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_HOTSPOT:
      if (!saveProgressBeforeRestart()) {
        resumeReadingStatsTimer("restart_save_failed");
        requestUpdate();
        return true;
      }
      activityManager.goToHotspotFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_SELECT_CHAPTER:
      openChapterSelection();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_CLIPPING:
      return false;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_QUICK_LOCK:
      return handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongBack);
    default:
      return false;
  }
}

bool XtcReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::HOME_READER || action == CrossPointSettings::SHORT_PWRBTN::BACK_HOME)
    return dispatchShortcutAction(action);
  if (action == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    QuickActions::showConfiguredPopup(
        quickActionsPopup, [this] { requestUpdate(); },
        [this](const auto quickAction) {
          mappedInput.setReaderTouchscreenOverride(false);
          dispatchShortcutAction(quickAction);
        },
        [](const auto quickAction) { return supportsQuickAction(quickAction); });
    if (quickActionsPopup.isActive()) {
      mappedInput.setReaderTouchscreenOverride(true);
      quickActionsPopup.setCancelCallback([this] { mappedInput.setReaderTouchscreenOverride(false); });
    }
    return true;
  }
  return executeReaderShortcutAction(action);
}

void XtcReaderActivity::render(RenderLock&&) {
  // Also drain on error/end screens and early returns, while the render lock
  // still protects the session. Normal pages drain before starting their timer.
  struct FlushStatsOnReturn {
    XtcReaderActivity& reader;
    ~FlushStatsOnReturn() { reader.flushDeferredReadingStats(false); }
  } flushStats{*this};

  showOpeningFeedback();
  if (!xtc) {
    return;
  }
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  const uint32_t pageToRender = currentPage;
  if (pageToRender >= xtc->getPageCount()) {
    // This is the sole creation and load site: its app and theme tokens are
    // absent during normal reading and allocation failure leaves an empty end screen.
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) {
        LOG_ERR("XTR", "OOM: EndOfBookOptions (%u bytes)", static_cast<unsigned>(sizeof(EndOfBookOptions)));
      }
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(xtc->getPath());
      endOfBookMenuActive.store(endOfBookOptions->menuActive(), std::memory_order_relaxed);
      mappedInput.setMenuMode(endOfBookOptions->menuActive());
      endOfBookOptions->render(renderer, mappedInput);
    }
    openingFeedback.finish();
    renderer.displayBuffer();
    return;
  }

  renderPage(pageToRender);
  openingFeedback.finish();
  pageShownAtMs = millis();
  startDailyReadingInterval();
  if (!queueProgressSave(pageToRender)) {
    LOG_ERR("XTR", "Failed to save debounced reader progress");
  }
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo(const uint32_t pageToRender,
                                                                     const bool includeTitle) const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(pageToRender) + 1;
  std::string title;

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  xtc::ChapterInfo chapter{};
  if (!xtc->getChapterForPage(pageToRender, chapter) || chapter.endPage < chapter.startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (includeTitle) title = chapter.name[0] == '\0' ? tr(STR_UNNAMED) : chapter.name;

  return StatusBarInfo{static_cast<int>(pageToRender - chapter.startPage) + 1,
                       static_cast<int>(chapter.endPage - chapter.startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position, const uint32_t pageToRender,
                                               const bool drawContent, const int batteryPercentage) const {
  const bool drawBottom = (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM ||
                           SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTH) &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_TOP ||
                        SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTH) &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const bool legacyTop = drawTop && SETTINGS.legacyXtcTopUsesBottom;
  const auto displayedBar = drawTop ? ReaderStatusBarPosition::Top : ReaderStatusBarPosition::Bottom;
  const auto configuredBar = xtcStatusBarConfigPosition(displayedBar, legacyTop);
  auto config =
      xtcStatusBarConfigForDisplay(displayedBar, legacyTop, SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top),
                                   SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom));
  const bool hidden = config.hidden;
  // Preserve the bitmap-strip clearing policy even for a persistently hidden bar.
  config.hidden = false;
  const int statusBarHeight = UITheme::getReaderStatusBarHeight(configuredBar, renderer, &config);
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = std::min(orientedMarginTop, UITheme::getTopStatusBarY(renderer));
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + UITheme::getInstance().getMetrics().topPadding +
                                    std::max(0, UITheme::getTopStatusBarInset(renderer)) + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  // XTC pages already contain a status strip in their bitmap. Clear that same
  // overlay area before returning so hiding it does not leave stale pixels.
  if (hidden || !statusBarVisible || !drawContent) {
    return;
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(pageToRender) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo(pageToRender, config.contains(ReaderStatusBarItem::TitleChapter));
  char bookTime[24] = {};
  char chapterTime[24] = {};
  const std::string bookTitle = config.contains(ReaderStatusBarItem::TitleBook) ? xtc->getTitle() : "";
  ReaderStatusBarContent content;
  content.batteryPercentage = batteryPercentage;
  content.bookProgress = progress;
  content.chapterPage = pageInfo.currentPage;
  content.chapterPageCount = pageInfo.pageCount;
  content.bookTitle = bookTitle.c_str();
  content.chapterTitle = pageInfo.title.c_str();
  content.timeLeftBook = config.contains(ReaderStatusBarItem::TimeLeftBook) &&
                                 formatTimeLeftLabel(bookTime, sizeof(bookTime), pageToRender, true)
                             ? bookTime
                             : nullptr;
  content.timeLeftChapter = config.contains(ReaderStatusBarItem::TimeLeftChapter) &&
                                    formatTimeLeftLabel(chapterTime, sizeof(chapterTime), pageToRender, false)
                                ? chapterTime
                                : nullptr;
  GUI.drawReaderStatusBar(renderer, displayedBar, content, legacyTop ? &config : nullptr);
}

void XtcReaderActivity::renderPage(const uint32_t pageToRender) {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();
  const auto renderOverlays = [this, pageToRender](const bool drawContent) {
    int batteryPercentage = -1;
    if (drawContent && statusBarVisible &&
        (SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top).contains(ReaderStatusBarItem::Battery) ||
         SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom).contains(ReaderStatusBarItem::Battery))) {
      batteryPercentage = powerManager.getBatteryPercentage();
    }
    renderStatusBarOverlay(StatusBarOverlayPosition::Top, pageToRender, drawContent, batteryPercentage);
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom, pageToRender, drawContent, batteryPercentage);
  };

  if (bitDepth == 2) {
    auto showStreamError = [&]() {
      renderer.clearScreen();
      const char* message =
          xtc->getLastError() == xtc::XtcError::MEMORY_ERROR ? tr(STR_MEMORY_ERROR) : tr(STR_PAGE_LOAD_ERROR);
      renderer.drawCenteredText(UI_12_FONT_ID, 300, message, true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
    };
    // XTCH stores two 48 KB planes. Stream each rendering pass through a 1 KB
    // scratch chunk so fragmented C3 heaps never need one contiguous 96 KB block.
    renderer.clearScreen();
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Base)) {
      showStreamError();
      return;
    }
    renderOverlays(true);
    openingFeedback.finish();

    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(pagesUntilFullRefresh < 0 ? manualScreenRefreshMode() : HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Lsb)) {
      showStreamError();
      return;
    }
    renderOverlays(false);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Msb)) {
      showStreamError();
      return;
    }
    renderOverlays(false);
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();

    renderer.clearScreen();
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Base)) {
      showStreamError();
      return;
    }
    renderOverlays(true);
    renderer.cleanupGrayscaleWithFrameBuffer();
    pluginEvents.rendered(xtc->getPageCount() ? static_cast<int>((pageToRender + 1ULL) * 10000 / xtc->getPageCount())
                                              : 0);
    return;
  }

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes.
  const size_t pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(pageToRender, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", pageToRender, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  showOpeningFeedback();
  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  // 1-bit mode: 8 pixels per byte, MSB first
  const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

  for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
    const size_t srcRowStart = srcY * srcRowBytes;

    for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
      // Read source pixel (MSB first, bit 7 = leftmost pixel)
      const size_t srcByte = srcRowStart + srcX / 8;
      const size_t srcBit = 7 - (srcX % 8);
      const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

      if (isBlack) {
        renderer.drawPixel(srcX, srcY, true);
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  renderOverlays(true);

  // Display with appropriate refresh
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  pluginEvents.rendered(xtc->getPageCount() ? static_cast<int>((pageToRender + 1ULL) * 10000 / xtc->getPageCount())
                                            : 0);
}

bool XtcReaderActivity::saveProgress(const uint32_t page) {
  if (!xtc) {
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    return false;
  }
  uint8_t data[4];
  data[0] = page & 0xFF;
  data[1] = (page >> 8) & 0xFF;
  data[2] = (page >> 16) & 0xFF;
  data[3] = (page >> 24) & 0xFF;
  const bool written = f.write(data, sizeof(data)) == sizeof(data);
  f.close();
  if (!written) {
    LOG_ERR("XTR", "Short write saving reader progress");
    return false;
  }
  progressSaveDebouncer.markPersisted(page);
  // XTC has no KOReader position; reading only changes its stats, and only with tracking on.
  if (statsTrackingActive) pendingSync.mark(xtc->getPath(), SyncPending::STATS_BIT);
  return true;
}

bool XtcReaderActivity::queueProgressSave(const uint32_t pageToRender) {
  if (!progressSaveDebouncer.observe(pageToRender)) {
    return true;
  }
  return saveProgress(pageToRender);
}

bool XtcReaderActivity::flushQueuedProgress() {
  return !progressSaveDebouncer.hasPending() || saveProgress(progressSaveDebouncer.lastObservedPosition());
}

void XtcReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

bool XtcReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Xtc xtc(filePath, "/.crosspoint");
  if (!xtc.load()) {
    LOG_DBG("SLP", "XTC: failed to load %s", filePath.c_str());
    return false;
  }

  // Load saved page number
  uint32_t savedPage = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      savedPage = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }
    f.close();
  }
  if (savedPage >= xtc.getPageCount()) savedPage = 0;

  const uint16_t pageWidth = xtc.getPageWidth();
  const uint16_t pageHeight = xtc.getPageHeight();
  const uint8_t bitDepth = xtc.getBitDepth();

  // Only use the 1-bit BW path; grayscale is not needed as a background under the overlay
  const size_t pageBufferSize = (bitDepth == 2) ? ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2
                                                : ((pageWidth + 7) / 8) * pageHeight;

  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("SLP", "XTC: failed to allocate page buffer");
    return false;
  }

  if (xtc.loadPage(savedPage, pageBuffer, pageBufferSize) == 0) {
    LOG_ERR("SLP", "XTC: failed to load page %lu", savedPage);
    free(pageBuffer);
    return false;
  }

  renderer.clearScreen();

  if (bitDepth == 2) {
    // 2-bit XTH: draw all non-white pixels as black (BW pass only)
    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const size_t colIndex = pageWidth - 1 - x;
        const size_t byteInCol = y / 8;
        const size_t bitInByte = 7 - (y % 8);
        const size_t byteOffset = colIndex * colBytes + byteInCol;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        if ((bit1 << 1) | bit2) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
  } else {
    // 1-bit XTG: draw black pixels
    const size_t srcRowBytes = (pageWidth + 7) / 8;
    for (uint16_t srcY = 0; srcY < pageHeight; srcY++) {
      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        const bool isBlack = !((pageBuffer[srcY * srcRowBytes + srcX / 8] >> (7 - srcX % 8)) & 1);
        if (isBlack) renderer.drawPixel(srcX, srcY, true);
      }
    }
  }

  free(pageBuffer);
  return true;
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}

void XtcReaderActivity::startDailyReadingInterval() {
  // Shortcuts can reach this without a lock; rendering/page turns already hold it.
  std::optional<RenderLock> statsLock;
  if (!RenderLock::heldByCurrentTask()) statsLock.emplace(*this);
  ReadingStatsDateTime local;
  if (pageShownAtMs != 0) getCurrentLocalDailyReadingDateTime(local);
  // Capture the new page clock before storage, but drain with the old start
  // time before replacing it. Storage time is spent reading the visible page.
  flushDeferredReadingStats(false);
  dailyReadingSession.start(local, SETTINGS.clockUtcOffsetQ);
  readingSessionDates.start(local, SETTINGS.clockUtcOffsetQ);
  readingSessionJournal.start(local, SETTINGS.clockUtcOffsetQ);
}

bool XtcReaderActivity::checkpointReadingStatsBeforeRestart() {
  recordCurrentPageReadingTime("restart_checkpoint");
  bool saved = readingSessionJournal.checkpoint();
  if (xtc && (statsTrackingActive || pendingStatsCommit) &&
      !saveReadingSessionCheckpoint(stats, globalStats, sessionReadingSeconds, &readingSessionDates,
                                    xtc->getCachePath())) {
    saved = false;
  }
  if (!saved) LOG_ERR("READER", "Cannot save reading stats before restart; retaining the live session");
  return saved;
}

bool XtcReaderActivity::saveProgressBeforeRestart() {
  // Silent network-mode restarts skip onExit(); coordinate with the render task.
  RenderLock lock(*this);
  bool saved = checkpointReadingStatsBeforeRestart();
  if (!(flushQueuedProgress())) {
    LOG_ERR("READER", "Failed to save progress before restart");
    saved = false;
  }
  return saved;
}
