#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Bluetooth Settings Activity
 * Allows users to toggle Bluetooth LE on/off
 */
class BluetoothSettingsActivity final : public Activity {
 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool bluetoothEnabled = false;
  const int MENU_ITEM_COUNT = 3;
};
