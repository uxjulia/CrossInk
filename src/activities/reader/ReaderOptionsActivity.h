#pragma once
#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "util/ButtonNavigator.h"

class ReaderOptionsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;
  std::vector<SettingInfo> fontSettings;
  std::vector<SettingInfo> pageLayoutSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;
  SettingAction activeSubmenu = SettingAction::None;

  void rebuildSettingsList();
  void setCurrentSettings();
  StrId activeSubmenuTitleId() const;
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  void moveSelection(bool forward);
  bool currentSettingUsesOptionMenu(const SettingInfo& setting) const;
  void openEnumOptionPicker(const SettingInfo& setting);
  void openScreenMarginPicker(const SettingInfo& setting);
  void toggleCurrentSetting();
  void openLineHeightPicker();

 public:
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderOptions", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
