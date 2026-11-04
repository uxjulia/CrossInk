#pragma once

#include <HalGPIO.h>

#include <array>
#include <atomic>
#include <cstddef>

#include "util/EdgeSlide.h"
#include "util/ReleaseSuppression.h"

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  static constexpr size_t BUTTON_COUNT = static_cast<size_t>(Button::PageForward) + 1;
  enum class SwipeDir { None, Left, Right, Up, Down };
  using EdgeSlide = ::EdgeSlide::Direction;
  struct EdgeSlideProgress {
    EdgeSlide direction = EdgeSlide::None;
    int distance = 0;
    bool finished = false;
    bool leftEdgeBand = false;
  };

  struct CompletedSwipe {
    uint8_t contactCount = 0;
    int startX = 0;
    int startY = 0;
    int endX = 0;
    int endY = 0;
    SwipeDir direction = SwipeDir::None;
    unsigned long durationMs = 0;
  };

  struct CompletedRotation {
    float degrees = 0.0f;
    int centerX = 0;
    int centerY = 0;
    unsigned long durationMs = 0;
  };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  // A label can be passed as plain text or as a localized arrow fragment plus
  // text. The fragment may appear before or after the text, depending on the
  // locale and the direction of the label.
  struct Label {
    enum class Placement { None, Prefix, Suffix };

    // cppcheck-suppress noExplicitConstructor
    constexpr Label(const char* text = nullptr) : text(text) {}
    constexpr Label(const Placement placement, const char* arrow, const char* text)
        : placement(placement), arrow(arrow), text(text) {}

    static constexpr Label withPrefix(const char* arrow, const char* text) {
      return Label(Placement::Prefix, arrow, text);
    }
    static constexpr Label withSuffix(const char* arrow, const char* text) {
      return Label(Placement::Suffix, arrow, text);
    }

    Placement placement = Placement::None;
    const char* arrow = nullptr;
    const char* text = nullptr;
  };

  explicit MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  // Enable/disable reader-specific front button mapping.
  // Call with true in reader activity onEnter(), false in onExit().
  void setReaderMode(bool enabled) { readerMode = enabled; }
  // Menus follow the screen rotation independently of reader page-turn preferences.
  // Publish while holding RenderLock, before drawing can temporarily rotate the renderer.
  void setMenuMode(bool enabled);
  bool isMenuMode() const { return navigationContext.load(std::memory_order_relaxed) & MENU_MODE; }
  void setPowerAsConfirmInReaderMode(bool enabled) { powerAsConfirmInReaderMode = enabled; }
#if CROSSINK_APP_CAP_TOUCH
  void setReaderTouchscreenOverride(bool enabled) { readerTouchscreenOverride = enabled; }
  void setTouchscreenRecoveryOverride(bool enabled) { touchscreenRecoveryOverride = enabled; }
#else
  constexpr void setReaderTouchscreenOverride(bool) {}
  constexpr void setTouchscreenRecoveryOverride(bool) {}
#endif

  void update() const;
  void suppressNextBackRelease() { releaseSuppression.suppressBack(); }
  void suppressNextConfirmRelease() { releaseSuppression.suppressConfirm(); }
  void suppressNextPowerRelease() { releaseSuppression.suppressPower(); }
  void suppressNextSideRelease(Button button);
  void suppressNextFrontRelease(Button button);
  void suppressNextPowerConfirmRelease() { releaseSuppression.suppressPowerConfirm(); }
  bool isPowerReleaseSuppressed() const { return releaseSuppression.isPowerReleaseSuppressed(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  void injectRelease(Button button) const { injectedReleases[static_cast<size_t>(button)] = true; }
  bool hasInjectedRelease(Button button) const { return injectedReleases[static_cast<size_t>(button)]; }
  void clearInjectedReleases() const { injectedReleases.fill(false); }
  bool isPressed(Button button) const;
  // Physical state is intentionally independent of release suppression. Use it
  // when a destination activity must consume the release of the button that
  // opened it.
  bool isPhysicalPressed(Button button) const;
  const GfxRenderer& getRenderer() const { return renderer; }
  enum class RowTouch : uint8_t { None, Down, Tap };
#if CROSSINK_APP_CAP_TOUCH
  bool hasTouch() const;
  bool hasTouchHardware() const;
  // Multi-touch follows the same touch gate as all other screen input,
  // so it cannot bypass the Disable Touchscreen setting.
  bool supportsMultiTouch() const;
  bool getTwoFingerTouch(int& x1, int& y1, int& x2, int& y2) const;
  bool wasCompletedMultiTouchSwipe(CompletedSwipe& swipe) const;
  bool wasCompletedMultiTouchRotation(CompletedRotation& rotation) const;
  // Report a side-band drag while held and once on release/cancellation.
  bool getEdgeSlideProgress(EdgeSlideProgress& progress);
  void resetEdgeSlide() { edgeSlideSide = EdgeSlide::None; }
  // True on boards with a capacitive home key (X4 Pro), where the bottom-edge
  // up-swipe is the reader-menu gesture rather than the exit-to-home gesture.
  // The Home key has its own reader lock setting, so it remains available when
  // "Disable Touchscreen" turns off only screen touch input.
  bool hasHomeKey() const { return hasHomeKeyHardware(); }
  // Capability is deliberately separate from this reader-only input gate so
  // Home-key layouts remain available while the key is locked on reader pages.
  bool isHomeButtonLockedInReader() const;
  bool wasScreenTapped(int& x, int& y) const;
  // Also reports how long the finger was held before release.
  bool wasScreenTapped(int& x, int& y, unsigned long& heldMs) const;
  bool isScreenTouchLongPress(int& x, int& y, unsigned long thresholdMs) const;
  bool wasScreenLongPress(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchTapCandidate(int& x, int& y, unsigned long& heldMs) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Ignore the remainder of the active contact when a touch-down action
  // replaces the current activity before the finger lifts.
  void suppressCurrentTouchContact() {
    suppressNextTouchTap();
    gpio.suppressTouchContact();
#ifdef SIMULATOR
    suppressSimulatedTouchContact = simulatorTouch.pressed || simulatorTouch.releasedThisFrame;
#endif
  }
  bool wasItemTapped(int& id) const;
  bool wasItemTouchedDown(int& id) const;
  bool wasTabTapped(int& id) const;
  bool wasTabTouchedDown(int& id) const;
  bool wasCoverTapped(int& id) const;
  bool wasCoverTouchedDown(int& id) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;
  bool isListItemTouchLongPressed(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                                  bool hasSubtitle, unsigned long thresholdMs) const;
  void suppressNextTouchTap() { suppressTouchTap = true; }
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;
  SwipeDir wasSwipe() const;
  bool wasSwipeWithPoints(SwipeDir& direction, int& startX, int& startY, int& endX, int& endY) const;
  bool wasLeftEdgeGesture() const;
  // An upward swipe that starts in the lower edge band.
  bool wasBottomEdgeUpSwipe() const;
  // Exit-to-home intent. Boards with a capacitive home key (X4 Pro) use the
  // key's press edge; everywhere else it's the bottom-edge up-swipe.
  bool wasHomeGesture() const;
  // Deliver a delayed capacitive Home-key tap through the usual activity route.
  void queueDeferredHomeGesture() { deferredHomeGesture = true; }
  void clearDeferredHomeGesture() const { deferredHomeGesture = false; }
  // Contextual menu intent: X4 Pro and Picco use the bottom-edge up-swipe;
  // other boards keep the top-edge down-swipe.
  bool wasMenuGesture() const;
  // Frontlight quick panel: top-edge down-swipe on X4 Pro and Picco.
  bool wasLightPanelGesture() const;
  // Full-screen vertical reader gestures. These preserve each board's existing
  // action mapping, but intentionally do not apply to reader submenus or lists.
  bool wasReaderMenuGesture() const;
  bool wasReaderHomeGesture() const;
  bool wasReaderLightPanelGesture() const;
  // Reader-menu shortcut: a long press of the capacitive home key.
  bool wasReaderMenuHold() const;
#else
  constexpr bool hasTouch() const { return false; }
  constexpr bool hasTouchHardware() const { return false; }
  constexpr bool supportsMultiTouch() const { return false; }
  constexpr bool getTwoFingerTouch(int&, int&, int&, int&) const { return false; }
  constexpr bool wasCompletedMultiTouchSwipe(CompletedSwipe&) const { return false; }
  constexpr bool wasCompletedMultiTouchRotation(CompletedRotation&) const { return false; }
  constexpr bool getEdgeSlideProgress(EdgeSlideProgress&) { return false; }
  constexpr void resetEdgeSlide() {}
  constexpr bool hasHomeKey() const { return false; }
  constexpr bool isHomeButtonLockedInReader() const { return false; }
  constexpr bool wasScreenTapped(int&, int&) const { return false; }
  constexpr bool wasScreenTapped(int&, int&, unsigned long&) const { return false; }
  constexpr bool isScreenTouchLongPress(int&, int&, unsigned long) const { return false; }
  constexpr bool wasScreenLongPress(int&, int&) const { return false; }
  constexpr bool wasScreenTouchDown(int&, int&) const { return false; }
  constexpr bool isScreenTouchTapCandidate(int&, int&, unsigned long&) const { return false; }
  constexpr bool isScreenTouchHeld(int&, int&) const { return false; }
  constexpr void suppressCurrentTouchContact() {}
  constexpr bool wasItemTapped(int&) const { return false; }
  constexpr bool wasItemTouchedDown(int&) const { return false; }
  constexpr bool wasTabTapped(int&) const { return false; }
  constexpr bool wasTabTouchedDown(int&) const { return false; }
  constexpr bool wasCoverTapped(int&) const { return false; }
  constexpr bool wasCoverTouchedDown(int&) const { return false; }
  constexpr bool wasScreenTouchReleased() const { return false; }
  constexpr bool wasTapInRect(int, int, int, int) const { return false; }
  constexpr bool wasListItemTapped(int&, int, int, int, int, bool) const { return false; }
  constexpr bool wasListItemTouchedDown(int&, int, int, int, int, bool) const { return false; }
  constexpr bool isListItemTouchLongPressed(int&, int, int, int, int, bool, unsigned long) const { return false; }
  constexpr void suppressNextTouchTap() {}
  constexpr RowTouch rowTouch(int&, int, int, int, int = 0, int = INT32_MAX, int = 0) const { return RowTouch::None; }
  constexpr RowTouch colTouch(int&, int, int, int, int, int, int = 0) const { return RowTouch::None; }
  constexpr SwipeDir wasSwipe() const { return SwipeDir::None; }
  constexpr bool wasSwipeWithPoints(SwipeDir& direction, int&, int&, int&, int&) const {
    direction = SwipeDir::None;
    return false;
  }
  constexpr bool wasLeftEdgeGesture() const { return false; }
  constexpr bool wasBottomEdgeUpSwipe() const { return false; }
  constexpr bool wasHomeGesture() const { return false; }
  constexpr void queueDeferredHomeGesture() {}
  constexpr void clearDeferredHomeGesture() const {}
  constexpr bool wasMenuGesture() const { return false; }
  constexpr bool wasLightPanelGesture() const { return false; }
  constexpr bool wasReaderMenuGesture() const { return false; }
  constexpr bool wasReaderHomeGesture() const { return false; }
  constexpr bool wasReaderLightPanelGesture() const { return false; }
  constexpr bool wasReaderMenuHold() const { return false; }
#endif
  // Directions for tabbed menus, derived from the physical button layout.
  Button menuButton(Button direction) const;
  // Upper increases on vertical layouts; right increases on horizontal layouts.
  Button sliderIncreaseButton() const;
  Button sliderDecreaseButton() const { return sliderIncreaseButton() == Button::Up ? Button::Down : Button::Up; }
  // Non-consuming: queued edges, held keys/fingers, or input in this frame.
  bool hasPendingActivity() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  uint32_t buttonSampleTime() const;
  // True when the current navigation context swaps the logical front navigation buttons.
  bool isFrontNavButtonSwapActive() const;
  Label withBackArrow(const char* text) const;
  Label withPreviousPageArrow(const char* text) const;
  Label withNextPageArrow(const char* text) const;
  const char* resolveLabel(Label label) const;
  Labels mapLabels(Label back, Label confirm, Label previous, Label next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;
  // Returns the raw front button index that was released this frame (or -1 if none).
  int getReleasedFrontButton() const;
  bool isFrontButtonPressed(uint8_t buttonIndex) const;

#ifdef SIMULATOR
  void simulatorInjectPress(Button button);
  void simulatorInjectRelease(Button button);
  void simulatorClearInputFrame();
#if CROSSINK_APP_CAP_TOUCH
  void simulatorInjectTouchDown(int x, int y);
  void simulatorInjectTouchMove(int x, int y);
  void simulatorInjectTouchRelease(int x, int y);
#endif
#endif

 private:
  HalGPIO& gpio;
  const GfxRenderer& renderer;
  bool readerMode = false;
  static constexpr uint8_t MENU_MODE = 4;
  // One atomic snapshot keeps the input context and its stable screen rotation together.
  std::atomic<uint8_t> navigationContext{0};
  uint8_t menuOrientation() const { return navigationContext.load(std::memory_order_relaxed) & (MENU_MODE - 1); }
  bool powerAsConfirmInReaderMode = false;
#if CROSSINK_APP_CAP_TOUCH
  bool usesFrontlightSwipeLayout() const;
  bool readerTouchscreenOverride = false;
  bool touchscreenRecoveryOverride = false;
  EdgeSlide edgeSlideSide = EdgeSlide::None;
  int edgeSlideStartX = 0;
  int edgeSlideStartY = 0;
  int edgeSlideLastX = 0;
  int edgeSlideLastY = 0;
  bool edgeSlideQualified = false;
#endif
  mutable ReleaseSuppression releaseSuppression;
  mutable uint8_t suppressedFrontReleases = 0;
#ifdef SIMULATOR
  mutable uint8_t suppressedSimulatedFrontReleases = 0;
#endif
  mutable bool suppressPhysicalUpRelease = false;
  mutable bool suppressPhysicalDownRelease = false;
  static constexpr size_t LABEL_BUFFER_SIZE = 128;
  mutable std::array<std::array<char, LABEL_BUFFER_SIZE>, 4> labelBuffers{};
  // One-frame synthetic releases let a chord route through the existing
  // activity navigation path without allocating an event object.
  mutable std::array<bool, BUTTON_COUNT> injectedReleases{};
#if CROSSINK_APP_CAP_TOUCH
  mutable bool suppressTouchTap = false;
  mutable bool deferredHomeGesture = false;
#endif
#ifdef SIMULATOR
  std::array<bool, BUTTON_COUNT> simulatorPressed{};
  std::array<bool, BUTTON_COUNT> simulatorReleased{};
  std::array<bool, BUTTON_COUNT> simulatorHeld{};
  std::array<unsigned long, BUTTON_COUNT> simulatorPressStart{};
  std::array<unsigned long, BUTTON_COUNT> simulatorReleasedHeldTime{};
#if CROSSINK_APP_CAP_TOUCH
  struct SimulatorTouch {
    bool pressed = false;
    bool pressedThisFrame = false;
    bool releasedThisFrame = false;
    bool longPressFired = false;
    int startX = 0;
    int startY = 0;
    int currentX = 0;
    int currentY = 0;
    unsigned long startedAt = 0;
  };
  mutable SimulatorTouch simulatorTouch;
  mutable bool suppressSimulatedTouchContact = false;
#endif
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool wasPhysicallyReleased(Button button) const;
  void expireReleaseSuppressions() const;
  uint8_t mappedSideButtonFor(Button button) const;
  uint8_t mappedFrontButtonFor(Button button) const;
  bool shouldUsePowerAsConfirmFallback() const;
  bool shouldMirrorPowerAsConfirmHold() const;
#if CROSSINK_APP_CAP_TOUCH
  bool touchInputEnabled() const;
  bool hasHomeKeyHardware() const;
  bool wasBackGesture() const;
  bool wasTopEdgeDownSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords.
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasRegistryTargetTapped(uint8_t kind, int& id) const;
  bool wasRegistryTargetTouchedDown(uint8_t kind, int& id) const;
  bool wasFrontButtonHintTapped(uint8_t buttonIndex) const;
  bool wasFrontButtonHintTouchedDown(uint8_t buttonIndex) const;
  void rememberTouchHeldTime() const;
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
#else
  constexpr bool wasBackGesture() const { return false; }
  constexpr bool wasFrontButtonHintTapped(uint8_t) const { return false; }
  constexpr bool wasFrontButtonHintTouchedDown(uint8_t) const { return false; }
#endif
};
