/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "BookStatsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "QuickActions.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "XtcReaderMenuActivity.h"
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

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

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
  sessionReadingSeconds = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  if (!skipRecentBookUpdateOnEntry) {
    RECENT_BOOKS.addOrUpdateBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  }
  SleepCoverAssets::prepareXtc(*xtc);

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

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  commitReadingStats();

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
      std::make_unique<XtcReaderMenuActivity>(renderer, mappedInput, std::move(title), hasChapters, stats.isCompleted),
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
  if (!xtc) {
    return;
  }
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const bool shortcutPageTurn = shortcutPageTurnPending;
  shortcutPageTurnPending = false;
  const bool shortcutPreviousPage = shortcutPreviousPagePending;
  shortcutPreviousPagePending = false;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const auto statusBarMode = static_cast<CrossPointSettings::XTC_STATUS_BAR_MODE>(SETTINGS.xtcStatusBarMode);
  const bool tappedStatusBar =
      touch.tapped && ((statusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                        ReaderUtils::isTopStatusBarTap(renderer, touch.y, statusBarHeight)) ||
                       (statusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                        ReaderUtils::isBottomStatusBarTap(renderer, touch.y, statusBarHeight)));
  if (tappedStatusBar) {
    statusBarVisible = !statusBarVisible;
    requestUpdate();
    return;
  }

  // Paged back into the book: release the end screen app and its theme tokens.
  {
    RenderLock lock(*this);
    if (currentPage < xtc->getPageCount() && endOfBookOptions) {
      endOfBookOptions.reset();
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

  const bool sideLongPressSkipsChapter =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP;
  if (sideLongPressSkipsChapter) {
    const bool sidePrevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack);
    const bool sideNextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward);
    if (sideButtonLongPressHandled && (sidePrevReleased || sideNextReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::PageBack);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::PageForward);
    if (!sideButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      sideButtonLongPressHandled = true;

      bool goHome = false;
      bool needsUpdate = false;
      {
        RenderLock lock(*this);
        const uint32_t pageCount = xtc->getPageCount();
        if (currentPage >= pageCount) {
          if (nextLongPressed) {
            goHome = true;
          } else {
            currentPage = pageCount > 0 ? pageCount - 1 : 0;
            needsUpdate = true;
          }
        } else {
          uint32_t forwardReadSeconds = 0;
          const bool shouldRecordForwardRead =
              nextLongPressed && forwardPageReadElapsed(forwardReadSeconds, "side_long_press");
          recordCurrentPageReadingTime("side_long_press");
          if (prevLongPressed) {
            currentPage = currentPage >= 10 ? currentPage - 10 : 0;
          } else {
            currentPage += 10;
            if (currentPage >= pageCount) {
              currentPage = pageCount;
            }
            if (shouldRecordForwardRead) {
              recordForwardPageTurn(forwardReadSeconds, false);
            }
          }
          needsUpdate = true;
        }
      }
      if (goHome) {
        onGoHome();
        return;
      }
      if (needsUpdate) {
        requestUpdate();
      }
      return;
    }
  }

  // Side buttons fire on press only when long-press action is OFF.
  const bool sideUsePress = SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF;

  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool sidePrev = sideUsePress ? mappedInput.wasPressed(MappedInputManager::Button::PageBack)
                                     : mappedInput.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideNext = sideUsePress ? mappedInput.wasPressed(MappedInputManager::Button::PageForward)
                                     : mappedInput.wasReleased(MappedInputManager::Button::PageForward);
  const bool frontPrev = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = mappedInput.wasReleased(MappedInputManager::Button::Power);
  if (powerReleased && longPowerPageTurnHandled) {
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

  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == CrossPointSettings::FONT_SIZE_CHANGE;
  if (frontLongPressAction) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (SETTINGS.longPressButtonBehavior == CrossPointSettings::FONT_SIZE_CHANGE) {
        return;
      }

      bool goHome = false;
      bool needsUpdate = false;
      {
        RenderLock lock(*this);
        const uint32_t pageCount = xtc->getPageCount();
        if (currentPage >= pageCount) {
          if (nextLongPressed) {
            goHome = true;
          } else {
            currentPage = pageCount > 0 ? pageCount - 1 : 0;
            needsUpdate = true;
          }
        } else {
          uint32_t forwardReadSeconds = 0;
          const bool shouldRecordForwardRead =
              nextLongPressed && forwardPageReadElapsed(forwardReadSeconds, "front_long_press");
          recordCurrentPageReadingTime("front_long_press");
          if (prevLongPressed) {
            currentPage = currentPage >= 10 ? currentPage - 10 : 0;
          } else {
            currentPage += 10;
            if (currentPage >= pageCount) {
              currentPage = pageCount;
            }
            if (shouldRecordForwardRead) {
              recordForwardPageTurn(forwardReadSeconds, false);
            }
          }
          needsUpdate = true;
        }
      }
      if (goHome) {
        onGoHome();
        return;
      }
      if (needsUpdate) {
        requestUpdate();
      }
      return;
    }
  }

  const bool fromSideBtn = (sidePrev || sideNext) && !(frontPrev || frontNext);
  const bool fromTilt = tiltPrev || tiltNext;
  bool prevTriggered = tiltPrev || sidePrev || frontPrev;
  bool nextTriggered = tiltNext || sideNext || frontNext;
  prevTriggered = prevTriggered || touch.prev || shortcutPreviousPage;
  nextTriggered = nextTriggered || touch.next || shortcutPageTurn;

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Touch page turns deliberately ignore the physical-button long-press
  // settings. Keep those saved settings intact for a later move back to a
  // button device, but never let a held screen tap skip pages.
  const bool fromTouch = touch.prev || touch.next;
  const unsigned long heldMs = fromTouch ? touch.heldMs : mappedInput.getHeldTime();

  // XTC pages are fixed-size bitmaps, so the orientation long-press action is
  // consumed here instead of rotating/clipping the pre-rendered page image.
  if (fromSideBtn &&
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE &&
      heldMs > ReaderUtils::SKIP_HOLD_MS) {
    return;
  }

  // Mirror EPUB's manual-turn guard: the render task updates the panel
  // concurrently, so accepting another turn before it owns RenderLock can mix
  // two pages. The short time gap covers that request-to-render startup window.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  const unsigned long now = millis();
  if (RenderLock::peek() || (now - lastPageTurnTime) < kMinManualTurnGapMs) {
    return;
  }
  lastPageTurnTime = now;

  const bool skipPages =
      !fromTouch && !fromTilt && !powerPageTurn && heldMs > ReaderUtils::SKIP_HOLD_MS &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP);
  const int skipAmount = skipPages ? 10 : 1;

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
    } else if (prevTriggered) {
      recordCurrentPageReadingTime("page_back");
      if (currentPage >= static_cast<uint32_t>(skipAmount)) {
        currentPage -= skipAmount;
      } else {
        currentPage = 0;
      }
      needsUpdate = true;
    } else if (nextTriggered) {
      uint32_t forwardReadSeconds = 0;
      const bool shouldRecordForwardRead = forwardPageReadElapsed(forwardReadSeconds, "page_forward");
      recordCurrentPageReadingTime("page_forward");
      currentPage += skipAmount;
      if (currentPage >= pageCount) {
        currentPage = pageCount;  // Allow showing "End of book"
      }
      if (shouldRecordForwardRead) {
        recordForwardPageTurn(forwardReadSeconds, !skipPages);
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

bool XtcReaderActivity::handleTwoFingerSwipeAction(const CrossPointSettings::TWO_FINGER_SWIPE_ACTION) {
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

void XtcReaderActivity::resumeReadingStatsTimer(const char*) {
  if (xtc && currentPage < xtc->getPageCount()) {
    pageShownAtMs = millis();
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
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  const uint32_t elapsedSeconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  if (elapsedSeconds == 0) {
    return false;
  }

  const uint32_t thresholdSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  if (elapsedSeconds > thresholdSeconds) {
    return false;
  }

  seconds = elapsedSeconds;
  return true;
}

bool XtcReaderActivity::forwardPageReadElapsed(uint32_t& seconds, const char*) const {
  seconds = 0;
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
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

void XtcReaderActivity::recordCurrentPageReadingTime(const char* source) {
  uint32_t seconds = 0;
  if (currentPageReadingSecondsForStats(seconds, source)) {
    sessionReadingSeconds = sessionReadingSeconds > UINT32_MAX - seconds ? UINT32_MAX : sessionReadingSeconds + seconds;
  }
  pageShownAtMs = 0UL;
}

void XtcReaderActivity::recordForwardPageTurn(const uint32_t seconds, const bool recordPace) {
  if (recordPace) {
    stats.recordForwardPageRead(seconds);
  }
  stats.totalPagesTurned++;
  globalStats.totalPagesTurned++;
}

bool XtcReaderActivity::formatTimeLeftLabel(char* buf, const size_t len, const uint32_t pageToRender) const {
  if (!buf || len == 0 || !xtc ||
      SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE) {
    return false;
  }

  const bool bookEstimate = SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_BOOK;
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
      static_cast<uint64_t>(total - current) * static_cast<uint64_t>(stats.avgSecondsPerForwardPage);
  formatCompactReadingDuration(static_cast<uint32_t>(std::min<uint64_t>(estimatedSeconds, UINT32_MAX)), buf, len);
  return true;
}

void XtcReaderActivity::commitReadingStats() {
  if (!xtc || !SETTINGS.shouldTrackReadingStats()) {
    return;
  }

  recordCurrentPageReadingTime("reader_exit");
  const uint32_t elapsedSecs = sessionReadingSeconds;
  if (elapsedSecs >= 60) {
    stats.sessionCount++;
    globalStats.totalSessions++;
  }
  if (elapsedSecs >= 10) {
    stats.totalReadingSeconds += elapsedSecs;
    globalStats.totalReadingSeconds += elapsedSecs;
    if (hasSessionStartLocalDateTime) {
      stats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
      globalStats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
    }
    if (elapsedSecs >= 120 && !stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime) {
      stats.startDate = sessionStartLocalDateTime.date;
    }
  }
  stats.save(xtc->getCachePath());
  globalStats.save();
}

void XtcReaderActivity::resetCurrentBookStatsAfterDelete() {
  stats = BookReadingStats{};
  sessionReadingSeconds = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
}

void XtcReaderActivity::setBookCompleted(const bool isCompleted) {
  if (!xtc || stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  if (isCompleted) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(xtc->getCachePath());
  globalStats.save();
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
  if (!xtc) return {};

  BookReadingStats displayStats = stats;
  if (SETTINGS.shouldTrackReadingStats()) {
    displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - sessionReadingSeconds
                                           ? UINT32_MAX
                                           : displayStats.totalReadingSeconds + sessionReadingSeconds;
    uint32_t currentPageSeconds = 0;
    if (currentPageReadingSecondsForStats(currentPageSeconds, "frontlight_stats_preview")) {
      displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - currentPageSeconds
                                             ? UINT32_MAX
                                             : displayStats.totalReadingSeconds + currentPageSeconds;
    }
  }

  const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
  if (hasSyncedStats) {
    return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, xtc->getTitle(), xtc->getCachePath(),
                                                displayStats, getCurrentBookProgressPercent(), false, 0, globalStats,
                                                GlobalReadingStats::loadAggregated(globalStats));
  }
  return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, xtc->getTitle(), xtc->getCachePath(), displayStats,
                                              getCurrentBookProgressPercent(), false, 0, globalStats);
}

void XtcReaderActivity::onFrontlightPanelClosed() {
  globalStats = GlobalReadingStats::load();
  stats = xtc ? BookReadingStats::load(xtc->getCachePath()) : stats;
  resumeReadingStatsTimer("frontlight_panel_return");
  requestUpdate();
}

void XtcReaderActivity::openChapterSelection() {
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
    resumeReadingStatsTimer("chapter_selection_unavailable");
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, pageToSelect),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             RenderLock lock(*this);
                             currentPage = std::get<PageResult>(result.data).page;
                           }
                           resumeReadingStatsTimer("chapter_selection_return");
                           requestUpdate();
                         });
}

void XtcReaderActivity::openReadingStats() {
  auto bookStats = createFrontlightReadingStatsActivity();
  if (!bookStats) {
    resumeReadingStatsTimer("book_stats_no_book");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(bookStats), [this](const ActivityResult&) {
    if (xtc) stats = BookReadingStats::load(xtc->getCachePath());
    globalStats = GlobalReadingStats::load();
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
    case XtcReaderMenuActivity::MenuAction::READING_STATS:
      openReadingStats();
      break;
    case XtcReaderMenuActivity::MenuAction::TOGGLE_COMPLETED:
      setBookCompleted(!stats.isCompleted);
      resumeReadingStatsTimer("toggle_completed_return");
      requestUpdate();
      break;
    case XtcReaderMenuActivity::MenuAction::DELETE_STATS:
      deleteBookStats();
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
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
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
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      activityManager.goToFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      activityManager.goToCalibreWireless(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

bool XtcReaderActivity::executeLongPressBackAction() {
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
      activityManager.goToCalibreWireless(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(xtc ? xtc->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
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
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    return;
  }

  renderPage(pageToRender);
  pageShownAtMs = millis();
  if (!queueProgressSave(pageToRender)) {
    LOG_ERR("XTR", "Failed to save debounced reader progress");
  }
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo(const uint32_t pageToRender) const {
  const auto statusBar = SETTINGS.statusBarSpec();
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(pageToRender) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  xtc::ChapterInfo chapter{};
  if (!xtc->getChapterForPage(pageToRender, chapter) || chapter.endPage < chapter.startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (statusBar.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapter.name[0] == '\0' ? tr(STR_UNNAMED) : chapter.name;
  }

  return StatusBarInfo{static_cast<int>(pageToRender - chapter.startPage) + 1,
                       static_cast<int>(chapter.endPage - chapter.startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position,
                                               const uint32_t pageToRender) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  // XTC pages already contain a status strip in their bitmap. Clear that same
  // overlay area before returning so hiding it does not leave stale pixels.
  if (!statusBarVisible) {
    return;
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(pageToRender) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo(pageToRender);
  char timeLeftLabel[24] = {};
  const char* timeLeft =
      formatTimeLeftLabel(timeLeftLabel, sizeof(timeLeftLabel), pageToRender) ? timeLeftLabel : nullptr;
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title.c_str(), paddingBottom,
                    0, false, timeLeft);
}

void XtcReaderActivity::renderPage(const uint32_t pageToRender) {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  if (bitDepth == 2) {
    auto showStreamError = [&]() {
      renderer.clearScreen();
      const char* message =
          xtc->getLastError() == xtc::XtcError::MEMORY_ERROR ? tr(STR_MEMORY_ERROR) : tr(STR_PAGE_LOAD_ERROR);
      renderer.drawCenteredText(UI_12_FONT_ID, 300, message, true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
    };
    const auto clearHiddenStatusBar = [this, pageToRender] {
      if (statusBarVisible) {
        return;
      }
      if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
        renderStatusBarOverlay(StatusBarOverlayPosition::Top, pageToRender);
      } else {
        renderStatusBarOverlay(StatusBarOverlayPosition::Bottom, pageToRender);
      }
    };

    // XTCH stores two 48 KB planes. Stream each rendering pass through a 1 KB
    // scratch chunk so fragmented C3 heaps never need one contiguous 96 KB block.
    renderer.clearScreen();
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Base)) {
      showStreamError();
      return;
    }
    clearHiddenStatusBar();

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
    clearHiddenStatusBar();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Msb)) {
      showStreamError();
      return;
    }
    clearHiddenStatusBar();
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();

    renderer.clearScreen();
    if (!streamXtchRenderPass(*xtc, pageToRender, pageWidth, pageHeight, renderer, XtchRenderPass::Base)) {
      showStreamError();
      return;
    }
    clearHiddenStatusBar();
    renderer.cleanupGrayscaleWithFrameBuffer();
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

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top, pageToRender);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom, pageToRender);
  }

  // Display with appropriate refresh
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
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
