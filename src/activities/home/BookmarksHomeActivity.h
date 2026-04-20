#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class BookmarksHomeActivity final : public Activity {
 public:
  explicit BookmarksHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookmarksHome", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<BookmarkedBookEntry> books;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  void openBookmarkList(int bookIndex);
};
