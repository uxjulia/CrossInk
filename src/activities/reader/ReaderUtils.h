#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "ReaderStatusBarTapTarget.h"
#include "ReaderTiltPageTurn.h"
#include "components/UITheme.h"

namespace ReaderUtils {

constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
// Hold duration to delete a row in the bookmark and clipping lists. Shared so the
// same gesture cannot drift apart between the two lists.
constexpr unsigned long DELETE_HOLD_MS = 1000;
constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;
// Gap between the top reader bar and the first line of book text.
constexpr int8_t TOP_STATUS_BAR_TEXT_PADDING = 0;

inline bool isNavigationLongPressAction(const uint8_t action) {
  return action == CrossPointSettings::LONG_MENU_HOME_READER || action == CrossPointSettings::LONG_MENU_BACK_HOME ||
         action == CrossPointSettings::LONG_MENU_TOGGLE_PAGE_TURN_GESTURES;
}

inline bool dispatchNavigationLongPressAction(const uint8_t action) {
  if (action == CrossPointSettings::LONG_MENU_TOGGLE_PAGE_TURN_GESTURES)
    return dispatchShortcutAction(CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES);
  if (!isNavigationLongPressAction(action)) return false;
  return dispatchShortcutAction(action == CrossPointSettings::LONG_MENU_HOME_READER ? CrossPointSettings::HOME_READER
                                                                                    : CrossPointSettings::BACK_HOME);
}

inline bool isRtlBookLanguage(std::string_view tag) {
  if (tag.size() < 2 || (tag.size() > 2 && tag[2] != '-' && tag[2] != '_')) return false;
  const auto first = std::tolower(static_cast<unsigned char>(tag[0]));
  const auto second = std::tolower(static_cast<unsigned char>(tag[1]));
  return (first == 'h' && second == 'e') || (first == 'i' && second == 'w') || (first == 'a' && second == 'r') ||
         (first == 'f' && second == 'a');
}

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

inline bool shouldShowTopStatusBar(const GfxRenderer& renderer) {
  return UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer) > 0;
}

// Night Mode is applied by the display after normal-polarity reader content is
// rendered. Keep this compatibility helper for existing reader call sites.
inline bool readerDarkModeEnabled() { return false; }

inline uint8_t readerBackgroundColor() { return readerDarkModeEnabled() ? 0x00 : 0xFF; }

inline bool readerForegroundBlack() { return true; }

inline int getTopStatusBarReservedHeight(const GfxRenderer& renderer) {
  const int statusBarHeight = UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer);
  if (statusBarHeight <= 0) {
    return 0;
  }

  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  // Reader layout already includes the bezel margin; reserve only the remainder.
  return std::max(0, UITheme::getTopStatusBarY(renderer) + statusBarHeight - top);
}

inline bool bottomStatusBarHasTextLane() {
  const auto bar = SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom);
  return !bar.hidden && bar.hasTextItems(halClock.isAvailable());
}

inline int getReaderFooterReservedHeight(const GfxRenderer& renderer, const bool automaticPageTurnActive) {
  if (SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom).hidden) return SETTINGS.screenMarginVertical;
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight(renderer);
  if (automaticPageTurnActive && !bottomStatusBarHasTextLane()) {
    const int autoTurnBarHeight =
        readerStatusBarTotalHeight(ReaderStatusBarPosition::Bottom, true, UITheme::getProgressBarHeight(),
                                   UITheme::getReaderStatusBarTextHeight(renderer));
    return std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                    std::max(statusBarHeight, autoTurnBarHeight) + STATUS_BAR_TEXT_PADDING);
  }
  return std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                  static_cast<int>(statusBarHeight + STATUS_BAR_TEXT_PADDING));
}

inline uint8_t rotatedOrientation(const uint8_t orientation, const bool clockwise) {
  return clockwise ? (orientation + 1) % CrossPointSettings::ORIENTATION_COUNT
                   : (orientation + CrossPointSettings::ORIENTATION_COUNT - 1) % CrossPointSettings::ORIENTATION_COUNT;
}

inline uint8_t flippedOrientation(const uint8_t orientation) {
  return (orientation + 2) % CrossPointSettings::ORIENTATION_COUNT;
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

inline TouchPageTurn detectTouchPageTurn(const GfxRenderer& renderer, const MappedInputManager& input,
                                         const bool rtlBook = false) {
#if !CROSSINK_APP_CAP_TOUCH
  (void)renderer;
  (void)input;
  (void)rtlBook;
  return {false, false, false, 0, 0, 0};
#else
  TouchPageTurn result{false, false, false, 0, 0, 0};
  if (!input.hasTouch()) {
    return result;
  }

  const int width = renderer.getScreenWidth();
  if (width <= 0) {
    return result;
  }

  const auto allowsSwipe = [](const uint8_t gesture) {
    return gesture == CrossPointSettings::TAP_AND_SWIPE || gesture == CrossPointSettings::SWIPE_ONLY;
  };
  const auto allowsTap = [](const uint8_t gesture) {
    return gesture == CrossPointSettings::TAP_AND_SWIPE || gesture == CrossPointSettings::TAP_ONLY ||
           gesture == CrossPointSettings::INVERTED_TAP;
  };

  const auto swipe = input.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::None) {
    if (SETTINGS.disablePageTurnGestures) return result;
    result.prev = swipe == (rtlBook ? MappedInputManager::SwipeDir::Left : MappedInputManager::SwipeDir::Right) &&
                  allowsSwipe(SETTINGS.previousPageGesture);
    result.next = swipe == (rtlBook ? MappedInputManager::SwipeDir::Right : MappedInputManager::SwipeDir::Left) &&
                  allowsSwipe(SETTINGS.pageTurnGesture);
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
  // wasScreenTapped() already rejects swipe motion. Stationary edge taps stay
  // available for page turns; readers reserve only enabled status-bar targets.

  // Give the entire page tap area to the sole tap-enabled direction. When
  // both accept taps, RTL books and Inverted Tap each swap their shared zones.
  if (SETTINGS.disablePageTurnGestures) return result;
  const bool nextTaps = allowsTap(SETTINGS.pageTurnGesture);
  const bool previousTaps = allowsTap(SETTINGS.previousPageGesture);
  const bool invertedTaps = (SETTINGS.pageTurnGesture == CrossPointSettings::INVERTED_TAP ||
                             SETTINGS.previousPageGesture == CrossPointSettings::INVERTED_TAP) != rtlBook;
  const bool nextZone = invertedTaps ? x < (width * 2) / 3 : x >= width / 3;
  result.next = nextTaps && (!previousTaps || nextZone);
  result.prev = previousTaps && (!nextTaps || !nextZone);
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
  return ReaderStatusBarTapTarget::containsTop(y, renderer.getScreenHeight(), UITheme::getTopStatusBarY(renderer),
                                               statusBarHeight);
}

// Reader menu opens on its board-specific vertical swipe anywhere on the open
// page, or a long press of the capacitive home key (a short home tap still goes home).
inline bool isTouchMenuGesture(const MappedInputManager& input) {
  // The capacitive Home key is independent from screen touch. Its configured
  // long-press reader-menu action must still work when screen touch is disabled.
  return input.wasReaderMenuHold() || (input.hasTouch() && input.wasReaderMenuGesture());
}

// X4 Pro opens the reader menu with an upward swipe. Its top-edge downward
// swipe is the opposite gesture and dismisses the menu instead of opening the
// frontlight panel. Other touch boards retain their existing menu behavior.
inline bool isTouchMenuDismissGesture(const MappedInputManager& input) {
  return input.hasTouch() && input.hasHomeKey() && input.wasLightPanelGesture();
}

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const auto tilt = detectReaderTiltPageTurn(halTiltSensor, SETTINGS.tiltPageTurn, SETTINGS.tiltNextPageOnly);

  const bool frontPrev = input.wasReleased(MappedInputManager::Button::Left);
  const bool powerReleased = input.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              input.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool longPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                             input.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool powerTurn = shortPowerTurn || longPowerTurn;
  const bool frontNext = input.wasReleased(MappedInputManager::Button::Right) || powerTurn;

  // Side-button actions are resolved by ReaderButtonShortcuts in each reader.
  return {tilt.prev || frontPrev, tilt.next || frontNext, false, tilt.prev || tilt.next};
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
