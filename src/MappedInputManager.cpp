#include "MappedInputManager.h"

#include <algorithm>
#include <utility>

#include "CrossPointSettings.h"
#include "GlobalActions.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};

bool isReaderLandscapeOrientation() {
  return SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CW ||
         SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW;
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

SideLayoutMap mapSideLayoutForReaderOrientation(SideLayoutMap side, const bool readerMode) {
  if (readerMode && SETTINGS.sideButtonOrientationAware && isReaderLandscapeOrientation()) {
    std::swap(side.pageBack, side.pageForward);
  }
  return side;
}

#ifdef SIMULATOR
size_t buttonIndex(MappedInputManager::Button button) { return static_cast<size_t>(button); }
#endif

}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto side = mapSideLayoutForReaderOrientation(kSideLayouts[sideLayout], readerMode);

  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
  const ButtonIndex mappedBack = mapFrontButtonForReaderOrientation(btnBack, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedConfirm = mapFrontButtonForReaderOrientation(btnConfirm, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedLeft = mapFrontButtonForReaderOrientation(btnLeft, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedRight = mapFrontButtonForReaderOrientation(btnRight, btnLeft, btnRight, readerMode);

  switch (button) {
    case Button::Back:
      return (gpio.*fn)(mappedBack);
    case Button::Confirm:
      return (gpio.*fn)(mappedConfirm);
    case Button::Left:
      return (gpio.*fn)(mappedLeft);
    case Button::Right:
      return (gpio.*fn)(mappedRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::shouldUsePowerAsConfirmFallback() const { return !readerMode || powerAsConfirmInReaderMode; }

bool MappedInputManager::shouldMirrorPowerAsConfirmHold() const {
  return shouldUsePowerAsConfirmFallback() &&
         !isPowerButtonActionAvailableOutsideReader(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
}

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

    return shouldUsePowerAsConfirmFallback() &&
           !isPowerButtonActionAvailableOutsideReader(
               static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) &&
           gpio.wasPressed(HalGPIO::BTN_POWER);
  }

  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
#ifdef SIMULATOR
  if (simulatorReleased[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Back) {
    if (!mapButton(button, &HalGPIO::wasReleased)) {
      return false;
    }

    if (suppressBackRelease) {
      suppressBackRelease = false;
      return false;
    }

    return true;
  }

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasReleased)) {
      return true;
    }

    if (!shouldUsePowerAsConfirmFallback() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
      return false;
    }

    if (suppressPowerConfirmRelease) {
      suppressPowerConfirmRelease = false;
      return false;
    }

    const bool longPress = gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    const auto action = longPress ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn)
                                  : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
    return !isPowerButtonActionAvailableOutsideReader(action);
  }

  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::isPressed)) {
      return true;
    }

    if (!shouldMirrorPowerAsConfirmHold() || !gpio.isPressed(HalGPIO::BTN_POWER)) {
      return false;
    }

    return !isPowerButtonActionAvailableOutsideReader(
               static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) ||
           gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  }

  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::wasAnyPressed() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorPressed.begin(), simulatorPressed.end(), [](bool pressed) { return pressed; })) {
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
  return gpio.wasAnyReleased();
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
#ifdef SIMULATOR
  const unsigned long now = millis();
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    if (simulatorHeld[i] && simulatorPressStart[i] > 0) {
      heldTime = std::max(heldTime, now - simulatorPressStart[i]);
    }
  }
#endif
  return heldTime;
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
  const ButtonIndex mappedBack = mapFrontButtonForReaderOrientation(btnBack, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedConfirm = mapFrontButtonForReaderOrientation(btnConfirm, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedLeft = mapFrontButtonForReaderOrientation(btnLeft, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedRight = mapFrontButtonForReaderOrientation(btnRight, btnLeft, btnRight, readerMode);

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](ButtonIndex hw) -> const char* {
    if (hw == mappedBack) return back;
    if (hw == mappedConfirm) return confirm;
    if (hw == mappedLeft) return previous;
    if (hw == mappedRight) return next;
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
  simulatorPressed[idx] = false;
  simulatorReleased[idx] = true;
  simulatorHeld[idx] = false;
}

void MappedInputManager::simulatorClearInputFrame() {
  simulatorPressed.fill(false);
  simulatorReleased.fill(false);
}
#endif
