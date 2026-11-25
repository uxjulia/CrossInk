#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Sync Server settings: account, what to include in each sync, and progress
 * options. Works with CrossPoint Sync and KOReader-compatible servers.
 */
class KOReaderSettingsActivity final : public Activity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // FreeInkApp hosts the settings list (themed rows, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

  OptionPopup optionPopup;
  ButtonNavigator buttonNavigator;
  freeink::ui::ListNav listNav;

  int headerErrorRow = -1;
  size_t selectedIndex = 0;
  std::string rowValue;  // Scratch for the row being laid out; read only during that row's draw.

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int topIndex = 0;  // viewport scroll position, decoupled from the selection
  bool ignoreInitialConfirmRelease = false;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void provideRow(void* user, uint16_t row, freeink::ui::ListItem& item);
  void fillRow(uint16_t row, freeink::ui::ListItem& item);
  void moveSelection(size_t next);
  void buildListScreen(UiApp::ScreenType& screen);

  void handleSelection();
};
