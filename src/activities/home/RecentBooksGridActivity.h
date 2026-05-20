#pragma once

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

  explicit RecentBooksGridActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooksGrid", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BookState {
    RecentBook book;
    float progress = -1.0f;
    bool progressLoaded = false;
  };
  static constexpr int NO_PAGE_LOADED = -1;

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool longPressFired = false;
  std::vector<BookState> recentBooks;
  int loadedPageStart = NO_PAGE_LOADED;

  void loadRecentBooks();
  void loadPageCovers(int pageStart);
  void ensureProgressLoaded(int index);
  void reloadAfterBookAction();
  void promptDeleteBook(const RecentBook& book);
  void promptRemoveBook(const std::string& path, const std::string& title);
  void showBookActionMenu(int bookIndex, bool ignoreInitialConfirmRelease = false);
};
