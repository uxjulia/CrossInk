#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <vector>

#include "FrontlightPanelModel.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Frontlight quick panel (boards with a frontlight + home key, e.g. X4 Pro).
// Opened globally by the top-edge down-swipe: a top-anchored drop-down (only the
// upper third of the screen) with brightness and warmth sliders driving the
// light live, plus an on/off toggle with a frontlight icon. It renders as an overlay —
// the content underneath stays on screen below the panel, and a tap there
// dismisses it. State is persisted once on exit (SPIFFS write throttling).
class FrontlightPanelActivity final : public Activity {
  // The sun toggle, two sliders, and four -/+ targets need seven interaction
  // slots. Keep small headroom for the panel's fixed controls.
  using UiApp = freeink::ui::FreeInkApp<20, 7>;

  ButtonNavigator buttonNavigator;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  FrontlightPanelContext context;
  FrontlightDrawerState drawerState{};
  OptionPopup optionPopup;
  bool initialInversion = false;
  bool initialTouchscreenDisabled = false;
  bool pendingTouchscreenDisabled = false;
  bool ttfRenderingChanged = false;
  // Swallow the swipe/tap fallout of a slider drag so its release can't
  // trigger the back gesture and close the panel mid-adjustment.
  bool draggingSlider = false;
  // Bottom edge of the drop-down (px). Content lays out above it and the
  // handle occupies its final band. Set by render() before the app lays out.
  int panelBottom = 0;
  int16_t panelRowHeight = 0;
  int16_t panelSpaceSm = 0;
  int16_t panelSpaceLg = 0;
  freeink::ui::Rect drawerHandleRect{};
  std::vector<std::string> readerTitleLines;
  // Cleared by computePanelBottom() when the chapter line cannot fit beside
  // the controls (e.g. landscape at Large scale).
  bool showChapterLine = false;

  static void panelScreen(UiApp::ScreenType& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onQuickActionEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDismissEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildPanelScreen(UiApp::ScreenType& screen);
  void addStepSlider(UiApp::ScreenType& screen, const freeink::ui::Rect& row, uint8_t value,
                     freeink::ui::ActionId sliderAction, freeink::ui::ActionId stepAction);
  Rect homeButtonRect() const;
  // Height of the drop-down, derived from the content it holds (header +
  // sliders + toggle). Same layout math as buildPanelScreen so the frame,
  // content margin, and dismiss threshold all agree.
  int computePanelBottom();
  void prepareReaderDetailsLayout();
  void drawReaderDetails(freeink::ui::Screen<20>& screen);
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void toggleReaderTouchscreen();
  void close();
  void activateQuickAction(int index);
  void openSyncDialog();
  void closeSyncDialog();
  void openReadingStats();
  void openGlobalSettings();
  void drawHeader();
  bool showsBookProgress() const;
  const char* bookSummaryChapter() const;
  int bookSummaryHeight() const;
  void drawBookProgress();

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   FrontlightPanelContext context = {});
  void onEnter() override;
  void onExit() override;
  // Keep the visible sliders and exit-time save in sync with a global edge action.
  void onExternalFrontlightChange() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Let loop() dismiss handle swipes even when the drawer reaches the bottom
  // edge. Keep the physical Home control separate from swipe dismissal.
  bool allowGlobalHomeSwipeGesture() const override { return false; }
  // From an active reader, the Home control returns to the library; elsewhere
  // it dismisses the overlay back to the current screen.
  bool handleHomeGesture() override;
  bool requiresFreshBackdrop() const override { return true; }
#ifdef SIMULATOR
  freeink::ui::Rect simulatorQuickActionRect(int index) const;
  freeink::ui::Rect simulatorHandleRect() const { return drawerHandleRect; }
  bool simulatorShowsChapterLine() const { return showChapterLine; }
  Rect simulatorSyncOptionRect(const int index) const { return optionPopup.simulatorOptionRect(index); }
  bool simulatorSyncOptionDisabled(const int index) const { return optionPopup.simulatorOptionDisabled(index); }
  int simulatorContentBottom = 0;
  int simulatorActionBarTop = 0;
  void simulatorActivateQuickAction(int index) { activateQuickAction(index); }
#endif
};
