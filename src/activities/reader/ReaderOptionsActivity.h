#pragma once
#include <I18n.h>

#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "util/ButtonNavigator.h"

class ReaderOptionsActivity final : public Activity {
 public:
  using SaveSettingsCallback = void (*)(void* ctx);
  using SaveGlobalSettingsCallback = void (*)(void* ctx);
  using GlobalSettingsEditCallback = void (*)(void* ctx);

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;
  std::vector<SettingInfo> fontSettings;
  std::vector<SettingInfo> pageLayoutSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;
  SettingAction activeSubmenu = SettingAction::None;
  SaveSettingsCallback saveSettingsCallback = nullptr;
  void* saveSettingsContext = nullptr;
  SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  bool settingsDirty = false;
  bool stablePageNumbersAvailable = false;

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
  void persistReaderSettings();
  void persistGlobalSettings();
  void beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();

 public:
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 SaveSettingsCallback saveSettingsCallback = nullptr,
                                 void* saveSettingsContext = nullptr,
                                 SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr,
                                 void* saveGlobalSettingsContext = nullptr,
                                 GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr,
                                 void* beginGlobalSettingsEditContext = nullptr,
                                 GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr,
                                 void* endGlobalSettingsEditContext = nullptr, bool stablePageNumbersAvailable = false)
      : Activity("ReaderOptions", renderer, mappedInput),
        saveSettingsCallback(saveSettingsCallback),
        saveSettingsContext(saveSettingsContext),
        saveGlobalSettingsCallback(saveGlobalSettingsCallback),
        saveGlobalSettingsContext(saveGlobalSettingsContext),
        beginGlobalSettingsEditCallback(beginGlobalSettingsEditCallback),
        beginGlobalSettingsEditContext(beginGlobalSettingsEditContext),
        endGlobalSettingsEditCallback(endGlobalSettingsEditCallback),
        endGlobalSettingsEditContext(endGlobalSettingsEditContext),
        stablePageNumbersAvailable(stablePageNumbersAvailable) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
