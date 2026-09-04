#pragma once

class GfxRenderer;
class HalGPIO;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power };
  enum class SwipeDir { None, Left, Right, Up, Down };
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };
  struct Label {
    enum class Placement { None, Prefix, Suffix };

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

  MappedInputManager(HalGPIO&, const GfxRenderer& renderer) : renderer(renderer) {}

  const GfxRenderer& getRenderer() const { return renderer; }

  bool wasScreenTouchDown(int& x, int& y) const {
    if (!touchDown) return false;
    x = touchX;
    y = touchY;
    touchDown = false;
    return true;
  }

  bool wasScreenTapped(int& x, int& y) const {
    if (!touchRelease) return false;
    touchRelease = false;
    if (suppressTouchTap) {
      suppressTouchTap = false;
      return false;
    }
    x = touchX;
    y = touchY;
    return true;
  }

  SwipeDir wasSwipe() const {
    const SwipeDir result = swipe;
    swipe = SwipeDir::None;
    return result;
  }
  bool wasPressed(const Button button) const {
    if (button != Button::Confirm || !confirmPressed) return false;
    confirmPressed = false;
    return true;
  }
  bool wasReleased(const Button button) const {
    if (button != Button::Power || !powerReleased) return false;
    if (powerReleaseSuppressed) {
      return false;
    }
    return true;
  }
  bool isPressed(const Button button) const { return button == Button::Power && powerPressed; }
  Label withBackArrow(const char* text) const { return Label(text); }
  Label withPreviousPageArrow(const char* text) const { return Label(text); }
  Label withNextPageArrow(const char* text) const { return Label(text); }
  Labels mapLabels(const Label back, const Label confirm, const Label previous, const Label next) const {
    return {back.text, confirm.text, previous.text, next.text};
  }

  void suppressNextTouchTap() { suppressTouchTap = true; }
  void suppressNextConfirmRelease() {}
  void suppressNextBackRelease() {}
  void suppressNextPowerRelease() { powerReleaseSuppressed = true; }

  void injectTouchDown(const int x, const int y) {
    touchX = x;
    touchY = y;
    touchDown = true;
  }

  void injectTouchRelease(const int x, const int y) {
    touchX = x;
    touchY = y;
    touchRelease = true;
  }

  void injectSwipe(const SwipeDir direction) { swipe = direction; }

  void injectPowerConfirmPress() {
    confirmPressed = true;
    powerPressed = true;
  }

  void injectPowerConfirmRelease() {
    powerPressed = false;
    powerReleased = true;
  }

  void advanceInputFrame() {
    powerReleased = false;
    if (!powerPressed) powerReleaseSuppressed = false;
  }

  bool isPowerReleaseSuppressed() const { return powerReleaseSuppressed; }

 private:
  const GfxRenderer& renderer;
  mutable bool touchDown = false;
  mutable bool touchRelease = false;
  mutable bool suppressTouchTap = false;
  mutable bool confirmPressed = false;
  mutable bool powerPressed = false;
  mutable bool powerReleased = false;
  mutable bool powerReleaseSuppressed = false;
  mutable SwipeDir swipe = SwipeDir::None;
  mutable int touchX = 0;
  mutable int touchY = 0;
};
