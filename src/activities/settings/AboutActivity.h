#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <HalDeviceInfo.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class AboutActivity final : public Activity {
 public:
  AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowGlobalHomeSwipeGesture() const override { return !scopePopup.isActive(); }
#ifdef SIMULATOR
  int simulatorVisibleRows() const { return visibleRows; }
  int simulatorRowCount() const;
  int simulatorTopIndex() const { return topIndex; }
  StrId simulatorExportStatus() const { return exportStatus; }
  const Rect& simulatorExportButtonRect() const { return exportButtonRect; }
  const OptionPopup& simulatorScopePopup() const { return scopePopup; }
  bool simulatorScopePopupActive() const { return scopePopup.isActive(); }
  bool simulatorExportPressed() const { return app.touchActive(); }
  const char* simulatorFirstHeading() const { return simulatorHeading; }
  void simulatorSetFirstHeading(const char* text) {
    simulatorHeading = text;
    requestUpdate();
  }
  const HalDeviceInfo::Snapshot& simulatorSnapshot() const { return snapshot; }
#endif

 private:
  using UiApp = freeink::ui::FreeInkApp<1, 1>;
  ButtonNavigator buttonNavigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  HalDeviceInfo::Snapshot snapshot;
#if FREEINK_DEVICE_EEGO_A4
  char uid[9]{};
#endif
  // Reused by the render-only row provider; no strings/arrays per row or frame.
  char valueBuffer[96]{};
  OptionPopup scopePopup;  // At most three scope choices; reuse the existing popup lifecycle.
  Rect exportButtonRect{};
  bool uiReady = false;
  StrId exportStatus = StrId::STR_SUPPORT_EXPORT_SHORT;
  void chooseExportScope();
  void confirmExport(bool includeBook);
  static void onExport(const freeink::ui::ActionEvent& event, void* user);
  int topIndex = 0;
  int visibleRows = 1;
#ifdef SIMULATOR
  const char* simulatorHeading = nullptr;  // Long localized-heading fixture; absent from hardware builds.
#endif
  static void aboutScreen(UiApp::ScreenType& screen, void* user);
  static void provideRow(void* user, uint16_t index, freeink::ui::ListItem& item);
};
