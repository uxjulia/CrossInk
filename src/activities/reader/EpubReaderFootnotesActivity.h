#pragma once

#include <Epub/FootnoteEntry.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::vector<FootnoteEntry>& footnotes);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
#ifdef SIMULATOR
  friend struct ScreenCalibrationSmokeTest;
#endif
  const std::vector<FootnoteEntry>& footnotes;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void selectFootnote();
};
