#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

enum class FileBrowserAction : int { Delete = 0, PinFavorite = 1, UnpinFavorite = 2 };

class FileBrowserActionActivity final : public Activity {
 public:
  struct MenuItem {
    FileBrowserAction action;
    StrId labelId;
  };

  FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::vector<MenuItem> items, bool ignoreInitialConfirmRelease = false)
      : Activity("FileBrowserAction", renderer, mappedInput),
        title(std::move(title)),
        items(std::move(items)),
        ignoreConfirmRelease(ignoreInitialConfirmRelease) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  std::string title;
  std::vector<MenuItem> items;
  int selectedIndex = 0;
  bool ignoreConfirmRelease = false;
};
