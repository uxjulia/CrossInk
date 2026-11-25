#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"
#include "util/ReaderStatusBarConfig.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public Activity {
#ifdef SIMULATOR
  friend struct StatusBarFeatureSmokeTest;
#endif
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool readerContext = false,
                                     bool stablePageNumbersAvailable = false, bool displayContext = false)
      : Activity("StatusBarSettings", renderer, mappedInput),
        readerContext(readerContext),
        displayContext(displayContext),
        uiTarget(makeUiTarget(renderer)),
        app(uiTarget, uiTarget.deviceContext()) {
    (void)stablePageNumbersAvailable;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowGlobalHomeSwipeGesture() const override { return !optionPopup.isActive(); }
  bool handleHomeGesture() override;

 private:
  enum class View : uint8_t { Root, Top, Bottom };
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;

  int selectedIndex = 0;
  int visibleItemCount = 0;
  bool readerContext = false;
  bool displayContext = false;
  View view = View::Root;

  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget;  // Must precede app: the app holds a reference to it.
  UiApp app;
  std::atomic<bool> uiReady{false};
  freeink::ui::ListNav listNav;
  int visibleRows = 1;
  int topIndex = 0;
#ifdef SIMULATOR
  bool simulatorSelectedRowVisible = false;
#endif

  void handleSelection();
  void openOptionPicker();
  void goBack();
  ReaderStatusBarPosition selectedPosition() const;
  void refreshItemCount();
  int previewHeight() const;
  int topPreviewOriginY() const;
  Rect settingsHeaderRect() const;
  static void settingsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildSettingsScreen(UiApp::ScreenType& screen);
};
