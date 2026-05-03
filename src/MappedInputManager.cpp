#include "MappedInputManager.h"

#include "CrossPointSettings.h"

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

bool isGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return action == CrossPointSettings::SHORT_PWRBTN::SLEEP || action == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH;
}
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;

  switch (button) {
    case Button::Back:
      return (gpio.*fn)(useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack);
    case Button::Confirm:
      return (gpio.*fn)(useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm);
    case Button::Left:
      return (gpio.*fn)(useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft);
    case Button::Right:
      return (gpio.*fn)(useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight);
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

bool MappedInputManager::shouldUsePowerAsConfirmFallback() const { return !readerMode; }

bool MappedInputManager::shouldMirrorPowerAsConfirmHold() const {
  return shouldUsePowerAsConfirmFallback() &&
         !isGlobalPowerButtonAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasPressed)) {
      return true;
    }
    return shouldMirrorPowerAsConfirmHold() && gpio.wasPressed(HalGPIO::BTN_POWER);
  }

  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
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

    return shouldMirrorPowerAsConfirmHold() || gpio.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  }

  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const {
  if (button == Button::Confirm) {
    return mapButton(button, &HalGPIO::isPressed) ||
           (shouldMirrorPowerAsConfirmHold() && gpio.isPressed(HalGPIO::BTN_POWER));
  }

  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const uint8_t btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const uint8_t btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const uint8_t btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const uint8_t btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    if (hw == btnBack) return back;
    if (hw == btnConfirm) return confirm;
    if (hw == btnLeft) return previous;
    if (hw == btnRight) return next;
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
