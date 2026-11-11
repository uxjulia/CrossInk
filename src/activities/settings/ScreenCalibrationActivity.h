#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <ScreenInsets.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ScreenCalibrationActivity final : public Activity {
 public:
  ScreenCalibrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowGlobalHomeSwipeGesture() const override { return false; }
#ifdef SIMULATOR
  friend struct ScreenCalibrationSmokeTest;
  const ScreenInsets& simulatorDraft() const { return draft; }
  bool simulatorSaveFailed() const { return saveFailed; }
  bool simulatorHit(int x, int y, freeink::ui::Interaction& hit) const { return app.hitPublished(x, y, 1, hit); }
#endif

 private:
  using UiApp = freeink::ui::FreeInkApp<21, 1>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator navigator;
  ScreenInsets draft;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Portrait;
  int selected = 0;
  bool editing = false;
  bool saveFailed = false;
  bool uiReady = false;

  void activate();
  void navigate(int delta);
  static void calibrationScreen(UiApp::ScreenType& screen, void* user);
  static void onControl(const freeink::ui::ActionEvent& event, void* user);
};
