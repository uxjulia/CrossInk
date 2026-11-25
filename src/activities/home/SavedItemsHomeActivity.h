#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct SavedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t bookmarkCount = 0;
  uint16_t clippingCount = 0;
};

class SavedItemsHomeActivity final : public Activity {
 public:
  SavedItemsHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
#ifdef SIMULATOR
  friend struct BookmarkListSmokeTest;
#endif
  std::vector<SavedBookEntry> books;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool longPressOpenHandled = false;
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void reloadSavedBooks();
  void openSavedItems(int bookIndex);
  void openBookmarkList(const SavedBookEntry& entry);
  void openClippingList(const SavedBookEntry& entry);
  void showSavedKindMenu(int bookIndex);
  void showSavedBookActionMenu(int bookIndex, bool ignoreInitialConfirmRelease);
};
