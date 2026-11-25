#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <vector>

#include "../Activity.h"
#include "../settings/SettingsActivity.h"
#include "components/MenuNavigationNote.h"
#include "components/OptionPopup.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"

class ControlsOptionsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int settingsCount = 0;
  std::vector<SettingInfo> settings;
  std::vector<SettingInfo> powerSettings;
  std::vector<SettingInfo> homeButtonSettings;
  std::vector<SettingInfo> frontButtonSettings;
  std::vector<SettingInfo> sideButtonSettings;
  std::vector<SettingInfo> tapsGesturesSettings;
  std::vector<SettingInfo> twoFingerSwipeSettings;
  std::vector<SettingInfo> edgeGestureSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;
  SettingAction activeSubmenu = SettingAction::None;
  SettingAction parentSubmenu = SettingAction::None;
  OptionPopup optionPopup;
  MenuNavigationNote menuNavigationNote;

  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget;  // Must precede app: the app holds a reference to it.
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  void rebuildSettingsList();
  void setCurrentSettings();
  StrId activeSubmenuTitleId() const;
  void openSubmenu(SettingAction action);
  void closeSubmenu();
  void moveSelection(bool forward);
  bool currentSettingUsesOptionMenu(const SettingInfo& setting) const;
  void openEnumOptionPicker(const SettingInfo& setting);
  void toggleCurrentSetting();
  static void optionsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildOptionsScreen(UiApp::ScreenType& screen);

 public:
  explicit ControlsOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ControlsOptions", renderer, mappedInput),
        uiTarget(makeUiTarget(renderer)),
        app(uiTarget, uiTarget.deviceContext()) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return false; }
};
