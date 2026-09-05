#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "components/OptionPopup.h"

enum class FileBrowserAction : int {
  Delete = 0,
  PinFavorite = 1,
  UnpinFavorite = 2,
  SetSleepFolder = 3,
  ClearSleepFolder = 4,
  DeleteCache = 5,
  ToggleCompleted = 6,
  RemoveFromRecents = 7,
  DeleteStats = 8,
  ViewBookmarks = 9,
  ViewClippings = 10,
  DeleteBookmarks = 11,
  DeleteClippings = 12,
  EpubRenderMode = 13,
  ResetReaderSettings = 14,
  SendNearby = 15,
  PinBootFavorite = 16,
  UnpinBootFavorite = 17,
};

class FileBrowserActionActivity final : public Activity {
 public:
  struct MenuItem {
    FileBrowserAction action;
    StrId labelId;
  };

  FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::vector<MenuItem> items, bool ignoreInitialConfirmRelease = false,
                            bool ignoreOpeningTouchRelease = true);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void finishCancelled();

  std::string title;
  std::vector<MenuItem> items;
  std::vector<std::string> optionLabels;
  OptionPopup optionPopup;
  bool ignoreConfirmRelease = false;
  bool ignoreOpeningTouchRelease = true;
  bool ignoreTouchRelease = false;
  bool selectionMade = false;
};
