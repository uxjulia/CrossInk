#include "MappedInputManager.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "CrossPointSettings.h"
#include "DeviceCapabilities.h"
#include "GlobalActions.h"
#if CROSSINK_APP_CAP_TOUCH
#include "components/TouchRegistry.h"
#endif
#include "components/UITheme.h"
#ifdef SIMULATOR
#include "simulator/SimulatorHomeKeyInput.h"
#endif

namespace {
using ButtonIndex = uint8_t;
constexpr ButtonIndex kNoButton = UINT8_MAX;
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_HOME_GESTURE_FRAC_Y = 0.06f;
constexpr float READER_BOTTOM_EDGE_HOME_GESTURE_FRAC_Y = 0.12f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;

constexpr float bottomEdgeHomeGestureFraction(const bool readerMode) {
  return readerMode ? READER_BOTTOM_EDGE_HOME_GESTURE_FRAC_Y : BOTTOM_EDGE_HOME_GESTURE_FRAC_Y;
}

struct SideLayoutMap {
  ButtonIndex pageBackPrimary;
  ButtonIndex pageBackSecondary;
  ButtonIndex pageForwardPrimary;
  ButtonIndex pageForwardSecondary;
};

bool shouldSwapMenuFrontButtons(const GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::LandscapeCounterClockwise || orientation == GfxRenderer::PortraitInverted;
}

bool shouldSwapReaderSideButtons(const bool readerMode) {
  return readerMode && SETTINGS.sideButtonOrientationAware && SETTINGS.orientation != CrossPointSettings::PORTRAIT;
}

bool shouldSwapReaderFrontNavButtons(const CrossPointSettings::FRONT_BUTTON_ORIENTATION_AWARE orientationMode) {
  if (orientationMode == CrossPointSettings::FRONT_ORIENTATION_AWARE_OFF) {
    return false;
  }
  return SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CW ||
         SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW ||
         (orientationMode == CrossPointSettings::FRONT_ORIENTATION_AWARE_NAV_BUTTONS &&
          SETTINGS.orientation == CrossPointSettings::INVERTED);
}

ButtonIndex invertFrontButtonPosition(const ButtonIndex button) {
  switch (button) {
    case HalGPIO::BTN_BACK:
      return HalGPIO::BTN_RIGHT;
    case HalGPIO::BTN_CONFIRM:
      return HalGPIO::BTN_LEFT;
    case HalGPIO::BTN_LEFT:
      return HalGPIO::BTN_CONFIRM;
    case HalGPIO::BTN_RIGHT:
      return HalGPIO::BTN_BACK;
    default:
      return button;
  }
}

ButtonIndex mapFrontButtonForReaderOrientation(const ButtonIndex button, const ButtonIndex leftButton,
                                               const ButtonIndex rightButton, const bool readerMode) {
  if (!readerMode) {
    return button;
  }

  const auto orientationMode =
      static_cast<CrossPointSettings::FRONT_BUTTON_ORIENTATION_AWARE>(SETTINGS.frontButtonOrientationAware);

  if (orientationMode == CrossPointSettings::FRONT_ORIENTATION_AWARE_ALL_BUTTONS &&
      SETTINGS.orientation == CrossPointSettings::INVERTED) {
    return invertFrontButtonPosition(button);
  }

  if (shouldSwapReaderFrontNavButtons(orientationMode)) {
    if (button == leftButton) {
      return rightButton;
    }
    if (button == rightButton) {
      return leftButton;
    }
  }

  return button;
}

ButtonIndex mapSideButtonForReaderOrientation(const ButtonIndex button, const bool readerMode) {
  if (!shouldSwapReaderSideButtons(readerMode)) {
    return button;
  }
  if (button == HalGPIO::BTN_UP) {
    return HalGPIO::BTN_DOWN;
  }
  if (button == HalGPIO::BTN_DOWN) {
    return HalGPIO::BTN_UP;
  }
  return button;
}

#ifdef SIMULATOR
size_t buttonIndex(MappedInputManager::Button button) { return static_cast<size_t>(button); }
#endif

}  // namespace

void MappedInputManager::setMenuMode(const bool enabled) {
  navigationContext.store(static_cast<uint8_t>(renderer.getOrientation()) | (enabled ? MENU_MODE : 0),
                          std::memory_order_relaxed);
}

void MappedInputManager::update() const {
  gpio.update();
  expireReleaseSuppressions();
  for (const auto button : {HalGPIO::BTN_BACK, HalGPIO::BTN_CONFIRM, HalGPIO::BTN_LEFT, HalGPIO::BTN_RIGHT}) {
    if (!gpio.isPressed(button) && !gpio.wasReleased(button)) suppressedFrontReleases &= ~(1u << button);
  }
  if (!gpio.isPressed(HalGPIO::BTN_UP) && !gpio.wasReleased(HalGPIO::BTN_UP)) suppressPhysicalUpRelease = false;
  if (!gpio.isPressed(HalGPIO::BTN_DOWN) && !gpio.wasReleased(HalGPIO::BTN_DOWN)) suppressPhysicalDownRelease = false;
}

void MappedInputManager::suppressNextFrontRelease(const Button button) {
  const auto physical = mappedFrontButtonFor(button);
  if (physical != kNoButton) suppressedFrontReleases |= 1u << physical;
#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)] || simulatorReleased[buttonIndex(button)])
    suppressedSimulatedFrontReleases |= 1u << static_cast<uint8_t>(button);
#endif
}

void MappedInputManager::suppressNextSideRelease(const Button button) {
  const ButtonIndex physical = mappedSideButtonFor(button);
  if (physical == HalGPIO::BTN_UP)
    suppressPhysicalUpRelease = true;
  else
    suppressPhysicalDownRelease = true;
}

bool MappedInputManager::wasPhysicallyReleased(const Button button) const {
#ifdef SIMULATOR
  if (simulatorReleased[buttonIndex(button)]) {
    return true;
  }
#endif
  return mapButton(button, &HalGPIO::wasReleased);
}

void MappedInputManager::expireReleaseSuppressions() const {
  ReleaseSuppression::FrameState state;
  state.backHeld = isPhysicalPressed(Button::Back);
  state.backReleased = wasPhysicallyReleased(Button::Back);
  state.confirmHeld = isPhysicalPressed(Button::Confirm);
  state.confirmReleased = wasPhysicallyReleased(Button::Confirm);
  state.powerHeld = isPhysicalPressed(Button::Power);
  state.powerReleased = wasPhysicallyReleased(Button::Power);
  releaseSuppression.expireAfterReleaseFrame(state);
}

MappedInputManager::Button MappedInputManager::menuButton(const Button direction) const {
  const auto orientation = static_cast<GfxRenderer::Orientation>(menuOrientation());
  const bool landscape =
      orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::LandscapeCounterClockwise;
  // Keep the front pair's Up/Down role on opposite-edge devices when rotated.
  if (!deviceUsesHorizontalSideButtonsForMenus(gpio) && !landscape) return direction;
  switch (direction) {
    case Button::Left:
      return Button::Up;
    case Button::Right:
      return Button::Down;
    case Button::Up:
      return Button::Left;
    case Button::Down:
      return Button::Right;
    default:
      return direction;
  }
}

MappedInputManager::Button MappedInputManager::sliderIncreaseButton() const {
  const auto orientation = static_cast<GfxRenderer::Orientation>(menuOrientation());
  const bool landscape =
      orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::LandscapeCounterClockwise;
  const bool horizontal = deviceUsesHorizontalSideButtonsForMenus(gpio) != landscape;
  return horizontal ? Button::Down : Button::Up;
}

uint8_t MappedInputManager::mappedSideButtonFor(const Button button) const {
  const ButtonIndex physical = button == Button::Up ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
  const uint8_t context = navigationContext.load(std::memory_order_relaxed);
  if (!(context & MENU_MODE)) return mapSideButtonForReaderOrientation(physical, readerMode);
  const auto orientation = static_cast<GfxRenderer::Orientation>(context & (MENU_MODE - 1));
  const bool swap =
      orientation == GfxRenderer::PortraitInverted ||
      orientation == (deviceUsesHorizontalSideButtonsForMenus(gpio) ? GfxRenderer::LandscapeCounterClockwise
                                                                    : GfxRenderer::LandscapeClockwise);
  return swap ? (button == Button::Up ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP) : physical;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const ButtonIndex up = mapSideButtonForReaderOrientation(HalGPIO::BTN_UP, readerMode);
  const ButtonIndex down = mapSideButtonForReaderOrientation(HalGPIO::BTN_DOWN, readerMode);
  const SideLayoutMap side = {
      SETTINGS.sideButtonUpShort == CrossPointSettings::PREVIOUS_PAGE ? up : kNoButton,
      SETTINGS.sideButtonDownShort == CrossPointSettings::PREVIOUS_PAGE ? down : kNoButton,
      SETTINGS.sideButtonUpShort == CrossPointSettings::PAGE_TURN ? up : kNoButton,
      SETTINGS.sideButtonDownShort == CrossPointSettings::PAGE_TURN ? down : kNoButton,
  };
  const auto sideEvent = [&](const ButtonIndex physical) {
    if (physical == kNoButton) return false;
    if (fn == &HalGPIO::wasReleased && ((physical == HalGPIO::BTN_UP && suppressPhysicalUpRelease) ||
                                        (physical == HalGPIO::BTN_DOWN && suppressPhysicalDownRelease)))
      return false;
    return (gpio.*fn)(physical);
  };

  const ButtonIndex frontButton = mappedFrontButtonFor(button);

  switch (button) {
    case Button::Back:
    case Button::Confirm:
    case Button::Left:
    case Button::Right:
      return frontButton != kNoButton && !(suppressedFrontReleases & (1u << frontButton)) && (gpio.*fn)(frontButton);
    case Button::Up:
      // Menu directions follow physical placement; page turns retain the reader preference.
      return sideEvent(mappedSideButtonFor(Button::Up));
    case Button::Down:
      return sideEvent(mappedSideButtonFor(Button::Down));
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return sideEvent(side.pageBackPrimary) || sideEvent(side.pageBackSecondary);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return sideEvent(side.pageForwardPrimary) || sideEvent(side.pageForwardSecondary);
  }

  return false;
}

uint8_t MappedInputManager::mappedFrontButtonFor(const Button button) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
  // In CCW landscape the right end of the front row is the upper end.
  // In inverted portrait it is the left end. Keep Back/Confirm in their configured slots.
  const uint8_t context = navigationContext.load(std::memory_order_relaxed);
  if (context & MENU_MODE) {
    const bool swap = shouldSwapMenuFrontButtons(static_cast<GfxRenderer::Orientation>(context & (MENU_MODE - 1)));
    switch (button) {
      case Button::Back:
        return btnBack;
      case Button::Confirm:
        return btnConfirm;
      case Button::Left:
        return swap ? btnRight : btnLeft;
      case Button::Right:
        return swap ? btnLeft : btnRight;
      default:
        return kNoButton;
    }
  }
  const ButtonIndex mappedBack = mapFrontButtonForReaderOrientation(btnBack, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedConfirm = mapFrontButtonForReaderOrientation(btnConfirm, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedLeft = mapFrontButtonForReaderOrientation(btnLeft, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedRight = mapFrontButtonForReaderOrientation(btnRight, btnLeft, btnRight, readerMode);

  switch (button) {
    case Button::Back:
      return mappedBack;
    case Button::Confirm:
      return mappedConfirm;
    case Button::Left:
      return mappedLeft;
    case Button::Right:
      return mappedRight;
    default:
      return kNoButton;
  }
}

bool MappedInputManager::shouldUsePowerAsConfirmFallback() const { return !readerMode || powerAsConfirmInReaderMode; }

bool MappedInputManager::isFrontNavButtonSwapActive() const {
  const uint8_t context = navigationContext.load(std::memory_order_relaxed);
  if (context & MENU_MODE) {
    return shouldSwapMenuFrontButtons(static_cast<GfxRenderer::Orientation>(context & (MENU_MODE - 1)));
  }
  return readerMode && shouldSwapReaderFrontNavButtons(static_cast<CrossPointSettings::FRONT_BUTTON_ORIENTATION_AWARE>(
                           SETTINGS.frontButtonOrientationAware));
}

bool MappedInputManager::shouldMirrorPowerAsConfirmHold() const {
  return shouldUsePowerAsConfirmFallback() &&
         !isPowerButtonActionAvailableOutsideReader(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
}

#if CROSSINK_APP_CAP_TOUCH
bool MappedInputManager::touchInputEnabled() const {
  return gpio.hasTouch() && (!readerMode || !SETTINGS.disableReaderTouchscreen || readerTouchscreenOverride);
}

bool MappedInputManager::hasTouch() const { return touchInputEnabled(); }

bool MappedInputManager::hasTouchHardware() const { return gpio.hasTouch(); }

bool MappedInputManager::supportsMultiTouch() const { return touchInputEnabled() && gpio.supportsMultiTouch(); }

bool MappedInputManager::getTwoFingerTouch(int& x1, int& y1, int& x2, int& y2) const {
  if (!supportsMultiTouch()) return false;

  const auto touch = gpio.getTouchSnapshot();
  if (touch.count != 2 || touch.reportedCount != 2) return false;

  renderer.tapToLogical(touch.contacts[0].nx, touch.contacts[0].ny, x1, y1);
  renderer.tapToLogical(touch.contacts[1].nx, touch.contacts[1].ny, x2, y2);
  return true;
}

bool MappedInputManager::wasCompletedMultiTouchSwipe(CompletedSwipe& swipe) const {
  if (!touchInputEnabled()) return false;

  HalGPIO::CompletedMultiTouchSwipe source;
  if (!gpio.wasCompletedMultiTouchSwipe(source)) return false;

  swipe.contactCount = source.contactCount;
  swipe.durationMs = source.durationMs;
  renderer.tapToLogical(source.nxStart, source.nyStart, swipe.startX, swipe.startY);
  renderer.tapToLogical(source.nxEnd, source.nyEnd, swipe.endX, swipe.endY);
  const int dx = swipe.endX - swipe.startX;
  const int dy = swipe.endY - swipe.startY;
  if (std::abs(dx) >= std::abs(dy)) {
    swipe.direction = dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  } else {
    swipe.direction = dy < 0 ? SwipeDir::Up : SwipeDir::Down;
  }
  return true;
}

bool MappedInputManager::wasCompletedMultiTouchRotation(CompletedRotation& rotation) const {
  if (!touchInputEnabled()) return false;

  HalGPIO::CompletedMultiTouchRotation source;
  if (!gpio.wasCompletedMultiTouchRotation(source)) return false;

  rotation.degrees = source.degrees;
  rotation.durationMs = source.durationMs;
  renderer.tapToLogical(source.nxCenter, source.nyCenter, rotation.centerX, rotation.centerY);
  return true;
}

bool MappedInputManager::isHomeButtonLockedInReader() const {
  return readerMode && hasHomeKeyHardware() && !SETTINGS.homeButtonInReaderEnabled && !readerTouchscreenOverride;
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!touchInputEnabled()) return false;
  if (suppressTouchTap) {
    // The release edge may be observed after the new activity's first render. Clear
    // on either edge so a missed release cannot suppress a later deliberate tap.
    if (gpio.wasTouchActivity()) suppressTouchTap = false;
    return false;
  }
#ifdef SIMULATOR
  if (simulatorTouch.releasedThisFrame) {
    if (simulatorTouch.longPressFired) {
      simulatorTouch.longPressFired = false;
      return false;
    }
    // Like the SDK InputManager, a release that moved as far as a swipe (60 px) is not also a tap.
    constexpr int kTapReleaseSlopPx = 59;
    if (std::abs(simulatorTouch.currentX - simulatorTouch.startX) > kTapReleaseSlopPx ||
        std::abs(simulatorTouch.currentY - simulatorTouch.startY) > kTapReleaseSlopPx)
      return false;
    x = simulatorTouch.startX;
    y = simulatorTouch.startY;
    rememberTouchHeldTime();
    return true;
  }
#endif
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTapped(int& x, int& y, unsigned long& heldMs) const {
  if (!wasScreenTapped(x, y)) return false;
  heldMs = touchHeldOverrideMs;
  return true;
}

bool MappedInputManager::isScreenTouchLongPress(int& x, int& y, const unsigned long thresholdMs) const {
  unsigned long heldMs = 0;
  return isScreenTouchTapCandidate(x, y, heldMs) && heldMs >= thresholdMs;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  if (!touchInputEnabled()) return false;
#ifdef SIMULATOR
  if (suppressSimulatedTouchContact) return false;
  if (simulatorTouch.pressed && !simulatorTouch.longPressFired && millis() - simulatorTouch.startedAt >= 500UL) {
    simulatorTouch.longPressFired = true;
    x = simulatorTouch.startX;
    y = simulatorTouch.startY;
    return true;
  }
#endif
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!touchInputEnabled()) return false;
#ifdef SIMULATOR
  if (suppressSimulatedTouchContact) return false;
  if (simulatorTouch.pressedThisFrame) {
    x = simulatorTouch.startX;
    y = simulatorTouch.startY;
    return true;
  }
#endif
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchDown(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchTapCandidate(int& x, int& y, unsigned long& heldMs) const {
  if (!touchInputEnabled()) return false;
#ifdef SIMULATOR
  if (suppressSimulatedTouchContact) return false;
  if (simulatorTouch.pressed) {
    x = simulatorTouch.startX;
    y = simulatorTouch.startY;
    heldMs = millis() - simulatorTouch.startedAt;
    return true;
  }
#endif
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!touchInputEnabled()) return false;
#ifdef SIMULATOR
  if (suppressSimulatedTouchContact) return false;
  if (simulatorTouch.pressed) {
    x = simulatorTouch.currentX;
    y = simulatorTouch.currentY;
    return true;
  }
#endif
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasRegistryTargetTapped(const uint8_t kind, int& id) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         TouchRegistry::getInstance().hitTest(tx, ty, static_cast<TouchRegistry::Kind>(kind), id);
}

bool MappedInputManager::wasRegistryTargetTouchedDown(const uint8_t kind, int& id) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         TouchRegistry::getInstance().hitTest(tx, ty, static_cast<TouchRegistry::Kind>(kind), id);
}

bool MappedInputManager::wasItemTapped(int& id) const { return wasRegistryTargetTapped(TouchRegistry::Kind::Item, id); }

bool MappedInputManager::wasItemTouchedDown(int& id) const {
  return wasRegistryTargetTouchedDown(TouchRegistry::Kind::Item, id);
}

bool MappedInputManager::wasTabTapped(int& id) const { return wasRegistryTargetTapped(TouchRegistry::Kind::Tab, id); }

bool MappedInputManager::wasTabTouchedDown(int& id) const {
  return wasRegistryTargetTouchedDown(TouchRegistry::Kind::Tab, id);
}

bool MappedInputManager::wasCoverTapped(int& id) const {
  return wasRegistryTargetTapped(TouchRegistry::Kind::Cover, id);
}

bool MappedInputManager::wasCoverTouchedDown(int& id) const {
  return wasRegistryTargetTouchedDown(TouchRegistry::Kind::Cover, id);
}

bool MappedInputManager::wasFrontButtonHintTapped(const uint8_t buttonIndex) const {
  int id = -1;
  return wasRegistryTargetTapped(TouchRegistry::Kind::Button, id) && id == buttonIndex;
}

bool MappedInputManager::wasFrontButtonHintTouchedDown(const uint8_t buttonIndex) const {
  int id = -1;
  return wasRegistryTargetTouchedDown(TouchRegistry::Kind::Button, id) && id == buttonIndex;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  if (!wasScreenTapped(tx, ty)) return false;
  if (TouchRegistry::getInstance().hitTest(tx, ty, TouchRegistry::Item, index) && index >= 0 && index < itemCount) {
    return true;
  }
  return listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  if (!wasScreenTouchDown(tx, ty)) return false;
  if (TouchRegistry::getInstance().hitTest(tx, ty, TouchRegistry::Item, index) && index >= 0 && index < itemCount) {
    return true;
  }
  return listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::isListItemTouchLongPressed(int& index, const int itemCount, const int selectedIndex,
                                                    const int listTop, const int listHeight, const bool hasSubtitle,
                                                    const unsigned long thresholdMs) const {
  int tx = 0;
  int ty = 0;
  if (!isScreenTouchLongPress(tx, ty, thresholdMs)) return false;
  if (TouchRegistry::getInstance().hitTest(tx, ty, TouchRegistry::Item, index) && index >= 0 && index < itemCount) {
    return true;
  }
  return listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!touchInputEnabled()) return false;
#ifdef SIMULATOR
  if (suppressSimulatedTouchContact) return false;
  if (simulatorTouch.releasedThisFrame) {
    sx = simulatorTouch.startX;
    sy = simulatorTouch.startY;
    ex = simulatorTouch.currentX;
    ey = simulatorTouch.currentY;
    return sx != ex || sy != ey;
  }
#endif
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

bool MappedInputManager::wasSwipeWithPoints(SwipeDir& direction, int& startX, int& startY, int& endX, int& endY) const {
  if (!decodeSwipe(startX, startY, endX, endY)) {
    direction = SwipeDir::None;
    return false;
  }

  const int dx = endX - startX;
  const int dy = endY - startY;
  if (std::abs(dx) >= std::abs(dy)) {
    direction = dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  } else {
    direction = dy < 0 ? SwipeDir::Up : SwipeDir::Down;
  }
  return true;
}

bool MappedInputManager::getEdgeSlideProgress(EdgeSlideProgress& progress) {
  progress = {};
  if (!touchInputEnabled()) {
    progress.finished = edgeSlideSide != EdgeSlide::None;
    edgeSlideSide = EdgeSlide::None;
    return progress.finished;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  if (width < 2 || height < 2) {
    progress.finished = edgeSlideSide != EdgeSlide::None;
    edgeSlideSide = EdgeSlide::None;
    return progress.finished;
  }
  const int band = ::EdgeSlide::bandWidth(width);
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y)) {
    edgeSlideSide = x < band ? EdgeSlide::LeftUp : (x >= width - band ? EdgeSlide::RightUp : EdgeSlide::None);
    edgeSlideStartX = x;
    edgeSlideStartY = y;
    edgeSlideLastX = x;
    edgeSlideLastY = y;
    edgeSlideQualified = false;
  }

  if (edgeSlideSide == EdgeSlide::None) return false;
  if (gpio.supportsMultiTouch() && gpio.getTouchSnapshot().reportedCount > 1) {
    edgeSlideSide = EdgeSlide::None;
    progress.finished = true;
    return true;
  }
  if (isScreenTouchHeld(x, y)) {
    if ((edgeSlideSide == EdgeSlide::LeftUp && x >= band) ||
        (edgeSlideSide == EdgeSlide::RightUp && x < width - band)) {
      edgeSlideSide = EdgeSlide::None;
      progress.finished = true;
      progress.leftEdgeBand = true;
      return true;
    } else {
      edgeSlideLastX = x;
      edgeSlideLastY = y;
      progress.direction = ::EdgeSlide::directionFor(edgeSlideStartX, edgeSlideStartY, x, y, width, height);
      edgeSlideQualified = progress.direction != EdgeSlide::None;
      if (edgeSlideQualified) progress.distance = std::abs(y - edgeSlideStartY);
    }
    return true;
  }
  if (!wasScreenTouchReleased()) {
    edgeSlideSide = EdgeSlide::None;
    progress.finished = true;
    return true;
  }
  progress.finished = true;
  int startX = 0;
  int startY = 0;
  if (!decodeSwipe(startX, startY, x, y)) {
    if (!edgeSlideQualified) {
      edgeSlideSide = EdgeSlide::None;
      return true;
    }
    // The SDK reserves wasSwipe() for quick flicks. Use the last held point
    // for a slow drag that already passed the edge-slide threshold.
    x = edgeSlideLastX;
    y = edgeSlideLastY;
  }

  const EdgeSlide side = edgeSlideSide;
  if ((side == EdgeSlide::LeftUp && x >= band) || (side == EdgeSlide::RightUp && x < width - band)) {
    edgeSlideSide = EdgeSlide::None;
    progress.leftEdgeBand = true;
    return true;
  }
  progress.direction = ::EdgeSlide::directionFor(edgeSlideStartX, edgeSlideStartY, x, y, width, height);
  if (progress.direction != EdgeSlide::None) progress.distance = std::abs(y - edgeSlideStartY);
  edgeSlideSide = EdgeSlide::None;
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  SwipeDir direction = SwipeDir::None;
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  wasSwipeWithPoints(direction, sx, sy, ex, ey);
  return direction;
}

bool MappedInputManager::wasBackGesture() const {
  if (!touchInputEnabled()) return false;
  // A disabled previous-page swipe must not fall through to Back/Home.
  if (readerMode && SETTINGS.previousPageGesture != CrossPointSettings::TAP_AND_SWIPE &&
      SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY) {
    return false;
  }
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasLeftEdgeGesture() const { return wasBackGesture(); }

bool MappedInputManager::hasHomeKeyHardware() const {
#ifdef SIMULATOR
#ifdef SIMULATOR_DEVICE_X4_PRO
  return true;
#else
  return false;
#endif
#else
  return gpio.hasHomeKey();
#endif
}

bool MappedInputManager::wasTopEdgeDownSwipe() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge swipe).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBottomEdgeUpSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int screenHeight = renderer.getScreenHeight();
    const int bottomEdgeTop = screenHeight - static_cast<int>(screenHeight * bottomEdgeHomeGestureFraction(readerMode));
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

// Home-key boards (X4 Pro) rearrange the edge gestures: the capacitive key
// takes over "exit to home", the bottom edge (freed by the key) becomes the
// menu gesture, and the top edge opens the frontlight quick panel.
bool MappedInputManager::wasMenuGesture() const {
  return hasHomeKeyHardware() ? wasBottomEdgeUpSwipe() : wasTopEdgeDownSwipe();
}

bool MappedInputManager::wasReaderMenuGesture() const {
  const SwipeDir direction = wasSwipe();
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  return direction == SwipeDir::Up;
#else
  return hasHomeKeyHardware() ? direction == SwipeDir::Up : direction == SwipeDir::Down;
#endif
}

bool MappedInputManager::wasReaderHomeGesture() const {
  // X4 Pro's capacitive Home key remains the reader's Home action. Only the
  // touch gesture changes in reader mode: other touch boards use an upward
  // swipe across the page.
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  // Sticky uses swipe up for the bottom reader drawer and swipe down for the
  // reader-details panel. Home remains available from that panel's header.
  return false;
#else
  if (hasHomeKeyHardware()) {
    return wasHomeGesture();
  }
  return wasSwipe() == SwipeDir::Up;
#endif
}

bool MappedInputManager::wasReaderLightPanelGesture() const {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  return wasSwipe() == SwipeDir::Down;
#else
  return hasHomeKeyHardware() && wasSwipe() == SwipeDir::Down;
#endif
}

bool MappedInputManager::wasHomeGesture() const {
  if (!hasHomeKeyHardware()) return wasBottomEdgeUpSwipe();
  if (isHomeButtonLockedInReader()) {
    clearDeferredHomeGesture();
    return false;
  }
  if (SETTINGS.homeButtonTapAction != CrossPointSettings::HOME_BUTTON_BACK_HOME) return false;
  // A swipe starting on the lower bezel can also report a short capacitive Home
  // tap on the X4 Pro. The screen gesture belongs to the active list/reader, so
  // give it priority over the global Home route for this release frame.
  if (wasSwipe() != SwipeDir::None) return false;
  if (deferredHomeGesture) {
    deferredHomeGesture = false;
    return true;
  }
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasTapped();
#else
  return gpio.wasHomeKeyTapped();
#endif
}

bool MappedInputManager::wasReaderMenuHold() const {
  if (!hasHomeKeyHardware()) return false;
  if (isHomeButtonLockedInReader()) return false;
  if (SETTINGS.homeButtonLongPressAction != CrossPointSettings::HOME_BUTTON_READER_MENU) return false;
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasLongPressed();
#else
  return gpio.wasHomeKeyLongPressed();
#endif
}

bool MappedInputManager::wasLightPanelGesture() const { return hasHomeKeyHardware() && wasTopEdgeDownSwipe(); }
#endif

bool MappedInputManager::wasPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorPressed[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasPressed)) {
      return true;
    }
    if (wasFrontButtonHintTouchedDown(mappedFrontButtonFor(button))) {
      return true;
    }

    if (releaseSuppression.isPowerReleaseSuppressed()) return false;

    if (powerAsConfirmInReaderMode && gpio.wasPressed(HalGPIO::BTN_POWER)) {
      // The active reader popup owns this Power press. Keep its configured
      // short/long action from firing after the popup confirms on press.
      releaseSuppression.suppressPower();
      return true;
    }

    return shouldUsePowerAsConfirmFallback() &&
           !isPowerButtonActionAvailableOutsideReader(
               static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) &&
           gpio.wasPressed(HalGPIO::BTN_POWER);
  }

  if (button == Button::Back && wasBackGesture()) {
    return true;
  }

  const uint8_t frontButton = mappedFrontButtonFor(button);
  return mapButton(button, &HalGPIO::wasPressed) ||
         (frontButton != kNoButton && wasFrontButtonHintTouchedDown(frontButton));
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (injectedReleases[static_cast<size_t>(button)]) {
    return true;
  }
#ifdef SIMULATOR
  const bool simulatedRelease = simulatorReleased[buttonIndex(button)];
  if (suppressedSimulatedFrontReleases & (1u << static_cast<uint8_t>(button))) return false;
#else
  constexpr bool simulatedRelease = false;
#endif

  if (button == Button::Back) {
    if (wasBackGesture()) {
      return true;
    }

#if CROSSINK_APP_CAP_TOUCH
    if (!simulatedRelease && !mapButton(button, &HalGPIO::wasReleased) &&
        !wasFrontButtonHintTapped(mappedFrontButtonFor(button))) {
      return false;
    }
#else
    if (!simulatedRelease && !mapButton(button, &HalGPIO::wasReleased)) {
      return false;
    }
#endif

    if (releaseSuppression.consumeBackRelease()) {
      return false;
    }

    return true;
  }

  if (button == Button::Confirm) {
    if (simulatedRelease || mapButton(button, &HalGPIO::wasReleased) ||
        wasFrontButtonHintTapped(mappedFrontButtonFor(button))) {
      if (releaseSuppression.consumeConfirmRelease()) {
        return false;
      }
      return true;
    }

    if (!shouldUsePowerAsConfirmFallback() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
      return false;
    }

    if (releaseSuppression.consumeConfirmRelease()) {
      return false;
    }

    if (releaseSuppression.consumePowerConfirmRelease()) {
      return false;
    }

    const bool longPress = gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    const auto action = longPress ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn)
                                  : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
    return !isPowerButtonActionAvailableOutsideReader(action);
  }

  if (button == Button::Power) {
    const bool released = simulatedRelease || mapButton(button, &HalGPIO::wasReleased);
    if (!released) {
      // A release edge stays visible for one full input loop. Once that loop
      // has passed, drop a stale suppression before the next Power press.
      return false;
    }

    // InputManager exposes the same release edge to every caller in this
    // loop. Keep the suppression set until the edge expires so the global
    // shortcut dispatcher cannot consume it first and leave the reader's
    // later handler to run the configured short-Power action.
    if (releaseSuppression.consumePowerRelease()) {
      return false;
    }

    return true;
  }

#ifdef SIMULATOR
  if (simulatedRelease && (button == Button::Up || button == Button::Down)) {
    const ButtonIndex physical = mappedSideButtonFor(button);
    if ((physical == HalGPIO::BTN_UP && suppressPhysicalUpRelease) ||
        (physical == HalGPIO::BTN_DOWN && suppressPhysicalDownRelease))
      return false;
  }
#endif

  const uint8_t frontButton = mappedFrontButtonFor(button);
  return simulatedRelease || mapButton(button, &HalGPIO::wasReleased) ||
         (frontButton != kNoButton && wasFrontButtonHintTapped(frontButton));
}

bool MappedInputManager::isPressed(const Button button) const {
#ifdef SIMULATOR
  if (suppressedSimulatedFrontReleases & (1u << static_cast<uint8_t>(button))) return false;
#endif
  if (button == Button::Power && releaseSuppression.isPowerReleaseSuppressed()) {
    return false;
  }

#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::isPressed)) {
      return true;
    }

    if (releaseSuppression.isPowerReleaseSuppressed() || !shouldMirrorPowerAsConfirmHold() ||
        !gpio.isPressed(HalGPIO::BTN_POWER)) {
      return false;
    }

    return !isPowerButtonActionAvailableOutsideReader(
               static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) ||
           gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  }

  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::isPhysicalPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)]) {
    return true;
  }
#endif
  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::hasPendingActivity() const {
#ifndef SIMULATOR
  if (gpio.hasPendingButtonInput()) return true;
#else
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (simulatorPressed[i] || simulatorReleased[i]) return true;
  }
#if CROSSINK_APP_CAP_TOUCH
  if (simulatorTouch.pressed || simulatorTouch.pressedThisFrame || simulatorTouch.releasedThisFrame) return true;
#endif
#endif
  // Avoid mapped tap/release helpers: they can consume gesture suppression.
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased()) return true;
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (isPhysicalPressed(static_cast<Button>(i)) || injectedReleases[i]) return true;
  }
#if CROSSINK_APP_CAP_TOUCH
  float x = 0, y = 0;
  return gpio.wasTouchActivity() || gpio.isTouchHeldAt(x, y);
#else
  return false;
#endif
}

bool MappedInputManager::wasAnyPressed() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorPressed.begin(), simulatorPressed.end(), [](bool pressed) { return pressed; })) {
    return true;
  }
#endif
#if CROSSINK_APP_CAP_TOUCH
  int id = -1;
  if (wasRegistryTargetTouchedDown(TouchRegistry::Kind::Button, id)) {
    return true;
  }
#endif
  return gpio.wasAnyPressed();
}

bool MappedInputManager::wasAnyReleased() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorReleased.begin(), simulatorReleased.end(), [](bool released) { return released; })) {
    return true;
  }
#endif
#if CROSSINK_APP_CAP_TOUCH
  int id = -1;
  if (wasRegistryTargetTapped(TouchRegistry::Kind::Button, id)) {
    return true;
  }
#endif
  return gpio.wasAnyReleased();
}

uint32_t MappedInputManager::buttonSampleTime() const {
#ifdef SIMULATOR
  return millis();
#else
  return gpio.buttonSampleTime();
#endif
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
#if CROSSINK_APP_CAP_TOUCH
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    heldTime = touchHeldOverrideMs;
  } else {
    touchHeldOverrideValid = false;
  }
#endif
#ifdef SIMULATOR
  const unsigned long now = millis();
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    if (simulatorHeld[i] && simulatorPressStart[i] > 0) {
      heldTime = std::max(heldTime, now - simulatorPressStart[i]);
    } else if (simulatorReleased[i]) {
      // Match InputManager: the release frame still reports how long the
      // button was held, so long-press releases are not read as short taps.
      heldTime = std::max(heldTime, simulatorReleasedHeldTime[i]);
    }
  }
#endif
  return heldTime;
}

namespace {

bool isRightToLeftUiLanguage() { return I18N.isRightToLeft(); }

}  // namespace

MappedInputManager::Label MappedInputManager::withBackArrow(const char* text) const {
  if (isRightToLeftUiLanguage()) return Label::withSuffix(tr(STR_ARROW_RIGHT), text);
  return Label::withPrefix(tr(STR_ARROW_LEFT), text);
}

MappedInputManager::Label MappedInputManager::withPreviousPageArrow(const char* text) const {
  if (isRightToLeftUiLanguage()) return Label(text);
  return Label::withPrefix(tr(STR_ARROW_LEFT), text);
}

MappedInputManager::Label MappedInputManager::withNextPageArrow(const char* text) const {
  if (isRightToLeftUiLanguage()) return Label(text);
  return Label::withSuffix(tr(STR_ARROW_RIGHT), text);
}

const char* MappedInputManager::resolveLabel(const Label label) const {
  const char* text = label.text != nullptr ? label.text : "";
  if (label.placement == Label::Placement::None || label.arrow == nullptr) return text;

  auto& buffer = labelBuffers[0];
  if (label.placement == Label::Placement::Prefix) {
    std::snprintf(buffer.data(), buffer.size(), "%s%s", label.arrow, text);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%s%s", text, label.arrow);
  }
  return buffer.data();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const Label back, const Label confirm, const Label previous,
                                                         const Label next) const {
  const ButtonIndex mappedBack = mappedFrontButtonFor(Button::Back);
  const ButtonIndex mappedConfirm = mappedFrontButtonFor(Button::Confirm);
  const ButtonIndex mappedLeft = mappedFrontButtonFor(Button::Left);
  const ButtonIndex mappedRight = mappedFrontButtonFor(Button::Right);

  // Compose arrow labels into instance-owned buffers so returned pointers
  // remain valid until the next label mapping on this input manager.
  auto resolveLabelToBuffer = [&](const Label label, const size_t bufferIndex) -> const char* {
    const char* text = label.text != nullptr ? label.text : "";
    if (label.placement == Label::Placement::None || label.arrow == nullptr) return text;

    auto& buffer = labelBuffers[bufferIndex];
    if (label.placement == Label::Placement::Prefix) {
      std::snprintf(buffer.data(), buffer.size(), "%s%s", label.arrow, text);
    } else {
      std::snprintf(buffer.data(), buffer.size(), "%s%s", text, label.arrow);
    }
    return buffer.data();
  };
  const char* resolvedBack = resolveLabelToBuffer(back, 0);
  const char* resolvedConfirm = resolveLabelToBuffer(confirm, 1);
  const char* resolvedPrevious = resolveLabelToBuffer(previous, 2);
  const char* resolvedNext = resolveLabelToBuffer(next, 3);

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](ButtonIndex hw) -> const char* {
    if (hw == mappedBack) return resolvedBack;
    if (hw == mappedConfirm) return resolvedConfirm;
    if (hw == mappedLeft) return resolvedPrevious;
    if (hw == mappedRight) return resolvedNext;
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
#if CROSSINK_APP_CAP_TOUCH
  int id = -1;
  if (wasRegistryTargetTouchedDown(TouchRegistry::Kind::Button, id) && id >= HalGPIO::BTN_BACK &&
      id <= HalGPIO::BTN_RIGHT) {
    return id;
  }
#endif
  return -1;
}

int MappedInputManager::getReleasedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping for screens whose labels are fixed to physical slots.
  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
#if CROSSINK_APP_CAP_TOUCH
  int id = -1;
  if (wasRegistryTargetTapped(TouchRegistry::Kind::Button, id) && id >= HalGPIO::BTN_BACK && id <= HalGPIO::BTN_RIGHT) {
    return id;
  }
#endif
  return -1;
}

bool MappedInputManager::isFrontButtonPressed(const uint8_t buttonIndex) const { return gpio.isPressed(buttonIndex); }

#ifdef SIMULATOR
void MappedInputManager::simulatorInjectPress(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorPressed[idx] = true;
  simulatorReleased[idx] = false;
  simulatorHeld[idx] = true;
  simulatorPressStart[idx] = millis();
}

void MappedInputManager::simulatorInjectRelease(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorReleasedHeldTime[idx] =
      simulatorHeld[idx] && simulatorPressStart[idx] > 0 ? millis() - simulatorPressStart[idx] : 0;
  simulatorPressed[idx] = false;
  simulatorReleased[idx] = true;
  simulatorHeld[idx] = false;
}

void MappedInputManager::simulatorClearInputFrame() {
  for (size_t i = 0; i < simulatorPressed.size(); ++i) {
    if (!simulatorHeld[i]) suppressedSimulatedFrontReleases &= ~(1u << i);
  }
  simulatorPressed.fill(false);
  simulatorReleased.fill(false);
#if CROSSINK_APP_CAP_TOUCH
  const bool suppressedContactReleased = suppressSimulatedTouchContact && simulatorTouch.releasedThisFrame;
  simulatorTouch.pressedThisFrame = false;
  simulatorTouch.releasedThisFrame = false;
  simulatorTouch.longPressFired = false;
  if (suppressedContactReleased) {
    suppressSimulatedTouchContact = false;
    suppressTouchTap = false;
  }
#endif
}

#if CROSSINK_APP_CAP_TOUCH
void MappedInputManager::simulatorInjectTouchDown(const int x, const int y) {
  simulatorTouch.pressed = true;
  simulatorTouch.pressedThisFrame = true;
  simulatorTouch.releasedThisFrame = false;
  simulatorTouch.startX = x;
  simulatorTouch.startY = y;
  simulatorTouch.currentX = x;
  simulatorTouch.currentY = y;
  simulatorTouch.startedAt = millis();
}

void MappedInputManager::simulatorInjectTouchMove(const int x, const int y) {
  if (!simulatorTouch.pressed) return;
  simulatorTouch.currentX = x;
  simulatorTouch.currentY = y;
}

void MappedInputManager::simulatorInjectTouchRelease(const int x, const int y) {
  if (!simulatorTouch.pressed) return;
  simulatorTouch.currentX = x;
  simulatorTouch.currentY = y;
  simulatorTouch.pressed = false;
  simulatorTouch.releasedThisFrame = true;
}
#endif
#endif
