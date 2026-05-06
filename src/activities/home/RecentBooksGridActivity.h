#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksGridActivity final : public Activity {
 public:
  static constexpr int BOOKS_PER_PAGE = 9;  // 3 cols x 3 rows
  static constexpr int MAX_GRID_BOOKS = BOOKS_PER_PAGE * 2;
  static constexpr int COVER_HEIGHT = 180;
  static constexpr int COVER_WIDTH = 123;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;
  std::vector<RecentBook> recentBooks;
  int loadedPageStart = -1;

  void loadRecentBooks();
  void loadPageCovers(int pageStart);

 public:
  explicit RecentBooksGridActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooksGrid", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
