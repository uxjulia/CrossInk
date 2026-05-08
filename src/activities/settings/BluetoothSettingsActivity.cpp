#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "BluetoothManager.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

extern BluetoothManager bluetoothManager;  // From main.cpp

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  bluetoothEnabled = SETTINGS.bluetoothEnabled;
  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  Activity::onExit();
  SETTINGS.saveToFile();
}

void BluetoothSettingsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoBack();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == 0) {  // Toggle button
      bluetoothEnabled = !bluetoothEnabled;
      SETTINGS.bluetoothEnabled = bluetoothEnabled;
      
      if (bluetoothEnabled) {
        if (bluetoothManager.enable()) {
          LOG_INF("BLE", "Bluetooth enabled via settings");
        } else {
          LOG_ERR("BLE", "Failed to enable Bluetooth");
          bluetoothEnabled = false;
          SETTINGS.bluetoothEnabled = false;
        }
      } else {
        if (bluetoothManager.disable()) {
          LOG_INF("BLE", "Bluetooth disabled via settings");
        }
      }
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEM_COUNT;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = (selectedIndex - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
    requestUpdate();
  });
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Draw header
  renderer.drawText(UI_12_FONT_ID, metrics.topPadding, metrics.topPadding, "Bluetooth Settings");

  const auto contentY = metrics.topPadding + 40 + metrics.verticalSpacing;
  const int itemHeight = 40;
  const int itemSpacing = 15;
  int currentY = contentY;

  // Draw Bluetooth toggle
  std::string statusText = bluetoothEnabled ? "[ON]" : "[OFF]";
  if (selectedIndex == 0) {
    renderer.drawFilledRect(5, currentY - 5, pageWidth - 10, itemHeight + 10, 0xCCCC);
  }
  renderer.drawText(UI_12_FONT_ID, 20, currentY, (std::string("Bluetooth: ") + statusText).c_str());
  currentY += itemHeight + itemSpacing;

  // Draw connection status
  std::string connStatus =
      (bluetoothEnabled && bluetoothManager.isConnected()) ? "Connected" : "Not Connected";
  if (selectedIndex == 1) {
    renderer.drawFilledRect(5, currentY - 5, pageWidth - 10, itemHeight + 10, 0xCCCC);
  }
  renderer.drawText(UI_12_FONT_ID, 20, currentY, (std::string("Status: ") + connStatus).c_str());
  currentY += itemHeight + itemSpacing;

  // Draw back option
  if (selectedIndex == 2) {
    renderer.drawFilledRect(5, currentY - 5, pageWidth - 10, itemHeight + 10, 0xCCCC);
  }
  renderer.drawText(UI_12_FONT_ID, 20, currentY, "Back");

  // Draw instructions
  const auto confirmLabel = (selectedIndex == 0) ? "Toggle" : "Select";
  const auto labels = mappedInput.mapLabels("Back", confirmLabel, "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
