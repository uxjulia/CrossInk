#pragma once

#include <vector>

#include "components/themes/BaseTheme.h"

class GfxRenderer;

class ThemeStub {
 public:
  void drawButtonHints(const GfxRenderer&, const char*, const char*, const char*, const char*, bool) const {}
  void drawOptionPopup(const GfxRenderer&, const char*, const std::vector<std::string>&, int, bool, const char*,
                       const char*, bool, int, const char*, const char*, const std::vector<bool>&) const {}
};

class UITheme {
 public:
  static UITheme& getInstance() {
    static UITheme instance;
    return instance;
  }

  const ThemeMetrics& getMetrics() const { return metrics; }
  const ThemeStub& getTheme() const { return theme; }

 private:
  ThemeMetrics metrics{.buttonHintsHeight = 40,
                       .scrollBarWidth = 4,
                       .scrollBarRightOffset = 5,
                       .optionPopupItemSpacing = 8,
                       .optionPopupInnerPadding = 12,
                       .optionPopupSelectionHPadding = 8,
                       .optionPopupSelectionVPadding = 4,
                       .optionPopupTitleGap = 8,
                       .optionPopupOptionFontBold = false,
                       .optionPopupDialogSideMargin = 15};
  ThemeStub theme;
};

#define GUI UITheme::getInstance().getTheme()
