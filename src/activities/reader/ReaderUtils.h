#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>

#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "ReaderStatusBarTapTarget.h"
#include "components/UITheme.h"

namespace ReaderUtils {

constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
// Hold duration to delete a row in the bookmark and clipping lists. Shared so the
// same gesture cannot drift apart between the two lists.
constexpr unsigned long DELETE_HOLD_MS = 1000;
constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;
// Gap between the top clock status bar band and the first line of book text.
// Signed so negative values pull the text up toward the clock (unsigned would wrap
// a negative to a huge positive). Note the book-text top margin is
// std::max(screenMarginVertical, reservedClockHeight + TOP_CLOCK_TEXT_PADDING), so this only
// bites once reservedClockHeight + padding drops below the vertical-margin setting.
constexpr int8_t TOP_CLOCK_TEXT_PADDING = 0;

inline GfxRenderer::Orientation toRendererOrientation(const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      return GfxRenderer::Orientation::Portrait;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case CrossPointSettings::ORIENTATION::INVERTED:
      return GfxRenderer::Orientation::PortraitInverted;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  renderer.setOrientation(toRendererOrientation(orientation));
}

// Vertical position for the readers' full-screen fallback messages (empty chapter,
// page load error, out of bounds). Derived from the live screen height so the text
// stays centered in every orientation instead of sitting at a fixed portrait offset.
inline int messageCenterY(const GfxRenderer& renderer) { return renderer.getScreenHeight() / 2; }

inline bool shouldShowTopClockStatusBar() { return halClock.isAvailable() && SETTINGS.shouldShowClockInReader(); }

// Night Mode is applied by the display after normal-polarity reader content is
// rendered. Keep this compatibility helper for existing reader call sites.
inline bool readerDarkModeEnabled() { return false; }

inline uint8_t readerBackgroundColor() { return readerDarkModeEnabled() ? 0x00 : 0xFF; }

inline bool readerForegroundBlack() { return true; }

inline int getTopClockStatusBarHeight() {
  if (!shouldShowTopClockStatusBar()) {
    return 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::max(UITheme::getStatusBarHeight(), metrics.statusBarVerticalMargin);
}

inline int getTopClockStatusBarReservedHeight(const GfxRenderer& renderer) {
  const int statusBarHeight = getTopClockStatusBarHeight();
  if (statusBarHeight <= 0) {
    return 0;
  }

  return UITheme::getInstance().getMetrics().topPadding + UITheme::getTopStatusBarInset(renderer) + statusBarHeight;
}

inline int getReaderFooterReservedHeight(const bool automaticPageTurnActive) {
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    return std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                    static_cast<int>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                                     STATUS_BAR_TEXT_PADDING));
  }
  return std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                  static_cast<int>(statusBarHeight + STATUS_BAR_TEXT_PADDING));
}

inline uint8_t rotatedOrientation(const uint8_t orientation, const bool clockwise) {
  return clockwise ? (orientation + 1) % CrossPointSettings::ORIENTATION_COUNT
                   : (orientation + CrossPointSettings::ORIENTATION_COUNT - 1) % CrossPointSettings::ORIENTATION_COUNT;
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromSideBtn;
  bool fromTilt;
};

struct TouchPageTurn {
  bool prev;
  bool next;
  bool tapped;
  int x;
  int y;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(const GfxRenderer& renderer, const MappedInputManager& input) {
#if !CROSSINK_APP_CAP_TOUCH
  (void)renderer;
  (void)input;
  return {false, false, false, 0, 0, 0};
#else
  TouchPageTurn result{false, false, false, 0, 0, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  const int width = renderer.getScreenWidth();
  if (width <= 0) {
    return result;
  }

  const auto pageTurnGesture = static_cast<CrossPointSettings::PAGE_TURN_GESTURE>(SETTINGS.pageTurnGesture);
  const bool allowsSwipe =
      pageTurnGesture == CrossPointSettings::TAP_AND_SWIPE || pageTurnGesture == CrossPointSettings::SWIPE_ONLY;
  const bool allowsTap = pageTurnGesture == CrossPointSettings::TAP_AND_SWIPE ||
                         pageTurnGesture == CrossPointSettings::TAP_ONLY ||
                         pageTurnGesture == CrossPointSettings::INVERTED_TAP;

  const auto swipe = input.wasSwipe();
  if (allowsSwipe && swipe != MappedInputManager::SwipeDir::None) {
    // A horizontal reader swipe turns pages wherever it starts. Edge-only
    // navigation remains handled by the activities that explicitly use it.
    result.prev = swipe == MappedInputManager::SwipeDir::Right;
    result.next = swipe == MappedInputManager::SwipeDir::Left;
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }
  result.tapped = true;
  result.x = x;
  result.y = y;
  result.heldMs = input.getHeldTime();
  // Reserve the top/bottom gesture bands for vertical edge swipes. If the touch
  // controller loses part of a short edge swipe, do not reinterpret it as a page tap.
  if (!allowsTap || input.isInVerticalEdgeGestureZone(y)) {
    return result;
  }

  if (pageTurnGesture == CrossPointSettings::INVERTED_TAP) {
    const int nextZoneWidth = (width * 2) / 3;
    result.next = x < nextZoneWidth;
    result.prev = x >= nextZoneWidth;
    return result;
  }

  const int previousZoneWidth = width / 3;
  result.prev = x < previousZoneWidth;
  result.next = x >= previousZoneWidth;
  return result;
#endif
}

inline bool isBottomStatusBarTap(const GfxRenderer& renderer, const int y, const int statusBarHeight) {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  return ReaderStatusBarTapTarget::containsBottom(y, renderer.getScreenHeight(), orientedMarginBottom, statusBarHeight);
}

inline bool isTopStatusBarTap(const GfxRenderer& renderer, const int y, const int statusBarHeight) {
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  return ReaderStatusBarTapTarget::containsTop(y, renderer.getScreenHeight(), orientedMarginTop, statusBarHeight);
}

// Reader menu opens on its board-specific vertical swipe anywhere on the open
// page, or a long press of the capacitive home key (a short home tap still goes home).
inline bool isTouchMenuGesture(const MappedInputManager& input) {
  // The capacitive Home key is independent from screen touch. Its configured
  // long-press reader-menu action must still work when screen touch is disabled.
  return input.wasReaderMenuHold() ||
         (SETTINGS.touchReaderControls && input.hasTouch() && input.wasReaderMenuGesture());
}

// X4 Pro opens the reader menu with an upward swipe. Its top-edge downward
// swipe is the opposite gesture and dismisses the menu instead of opening the
// frontlight panel. Other touch boards retain their existing menu behavior.
inline bool isTouchMenuDismissGesture(const MappedInputManager& input) {
  return SETTINGS.touchReaderControls && input.hasTouch() && input.hasHomeKey() && input.wasLightPanelGesture();
}

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  // Side buttons fire on press only when long-press action is OFF (nothing to detect).
  const bool sideUsePress = SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF;

  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool sidePrev = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                     : input.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideNext = sideUsePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                                     : input.wasReleased(MappedInputManager::Button::PageForward);

  const bool frontPrev = input.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              input.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             input.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool frontNext = input.wasReleased(MappedInputManager::Button::Right) || powerTurn;

  // fromSideBtn is true when only side buttons contributed to this page turn.
  const bool fromSide = (sidePrev || sideNext) && !(frontPrev || frontNext);
  return {tiltPrev || sidePrev || frontPrev, tiltNext || sideNext || frontNext, fromSide, tiltPrev || tiltNext};
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  // A negative countdown is reserved for the explicit Refresh Screen shortcut.
  // Regular cadence cleanup remains a HALF refresh at 1. The X4 retains its
  // prior clean HALF waveform; other panels use their full waveform.
  const auto mode = pagesUntilFullRefresh < 0    ? manualScreenRefreshMode()
                    : pagesUntilFullRefresh <= 1 ? HalDisplay::HALF_REFRESH
                                                 : HalDisplay::FAST_REFRESH;
  if (async) {
    renderer.displayBufferAsync(mode);
  } else {
    renderer.displayBuffer(mode);
  }
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
