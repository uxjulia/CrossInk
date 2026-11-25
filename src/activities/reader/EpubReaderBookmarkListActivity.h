#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "components/OptionPopup.h"
#include "components/SwipeDeleteRow.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarkListActivity final : public Activity {
 public:
  EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
#ifdef SIMULATOR
  friend struct BookmarkListSmokeTest;
#endif
  // The store outlives this foreground screen; deletion updates this same vector.
  const BookmarkRecords& bookmarks;
  // SDK rowProvider consumes this buffer before requesting the next row.
  char rowProgress[16]{};
  int selectedIndex = 0;
  bool confirmingDelete = false;
  ButtonNavigator buttonNavigator;
  OptionPopup confirmPopup;
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  SwipeDeleteRow swipeDelete;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void provideRow(void* user, uint16_t index, freeink::ui::ListItem& item);
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onDeleteEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void selectBookmark();

  void deleteSelectedBookmark();
  void showBookmarkDeletePopup();
};
