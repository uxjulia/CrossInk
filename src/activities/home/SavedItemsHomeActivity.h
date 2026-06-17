#pragma once

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
  explicit SavedItemsHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SavedItemsHome", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<SavedBookEntry> books;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool longPressOpenHandled = false;

  void reloadSavedBooks();
  void openSavedItems(int bookIndex);
  void openBookmarkList(const SavedBookEntry& entry);
  void openClippingList(const SavedBookEntry& entry);
  void showSavedKindMenu(int bookIndex);
  void showSavedBookActionMenu(int bookIndex, bool ignoreInitialConfirmRelease);
};
