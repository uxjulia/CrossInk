#pragma once

#include <functional>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

enum class NetworkMode {
  JOIN_NETWORK,
  CONNECT_CALIBRE,
  CREATE_HOTSPOT,
  USB_DRIVE,
  NEARBY_BOOK_RECEIVE,
  NEARBY_STATS_SYNC,
  SYNC_BOOKS,
  SYNC_READING_STATS,
  SYNC_CLIPPINGS,
  OPDS_BROWSER
};

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "Nearby Stats Sync" - Sync reading stats directly with a nearby reader
 * - "Receive File" - Receive a file directly from another reader
 * - "Sync Books", "Sync Reading Stats", "Sync Clippings" - Sync Server uploads
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
 public:
  static constexpr int MAX_MENU_ROWS = 16;

 private:
#ifdef SIMULATOR
  friend struct PluginCatalogSmokeTest;
#endif
  // FreeInkApp hosts the mode list (themed rows, icons, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiHost = UiAppHost<14, 4>;  // up to 9 rows plus 2 section headings
  using UiApp = UiHost::App;

  ButtonNavigator buttonNavigator;

  // Rows hidden by settings (stats tracking, What to Sync) are left out when the
  // screen opens. selectedIndex counts visible rows; section headings are not rows.
  uint8_t menuEntries[MAX_MENU_ROWS]{};    // visible row -> MENU_ENTRIES index
  uint8_t menuListIndex[MAX_MENU_ROWS]{};  // visible row -> list index including headings
  int menuCount = 0;
  int listCount = 0;
  int selectedIndex = 0;
  bool buttonSelectionActive = false;

  UiHost ui;
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  freeink::ui::ListNav listNav;
  ScreenTransitionRefresh screenTransitionRefresh;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void selectCurrent();
  void buildMenu();
  int listIndexForMenuIndex(int menuIndex) const;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
