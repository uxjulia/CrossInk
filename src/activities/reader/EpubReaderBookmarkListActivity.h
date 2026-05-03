#pragma once

#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarkListActivity final : public Activity {
 public:
  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::vector<Bookmark>& bookmarks)
      : Activity("EpubReaderBookmarkList", renderer, mappedInput), bookmarks(bookmarks) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  std::vector<Bookmark> bookmarks;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  int getPageItems() const;
};
