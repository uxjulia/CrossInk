#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Edit screen for a single OPDS server.
 * Shows server credentials, filename options, download folder and Delete.
 * Used for both adding new servers and editing existing ones.
 */
class OpdsSettingsActivity final : public Activity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
#ifdef SIMULATOR
  friend struct OpdsCatalogSmokeTest;
#endif
  // FreeInkApp hosts the field list (themed rows, touch routing); the header
  // stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<12, 4>;

  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;
  bool showFolderError = false;
  std::string downloadFolderLabel;  // Built on edit, reused by render().

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  int getMenuItemCount() const;
  void handleSelection();
  bool saveServer();
  void editDownloadFolder();
  void setDownloadFolder(std::string folder);
  void updateDownloadFolderLabel();
};
