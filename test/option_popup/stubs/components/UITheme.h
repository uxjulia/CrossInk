#pragma once

#include <vector>

#include "components/OptionLabels.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

class ThemeStub {
 public:
  void drawButtonHints(const GfxRenderer&, const char*, const char*, const char*, const char*, bool) const {}
  void drawOptionPopup(const GfxRenderer&, const char*, OptionLabels, const int selectedIndex, bool, const char*,
                       const char*, bool, int, const char* noteLabel, const char* noteBody, const int firstOptionIndex,
                       const char* secondNoteLabel = nullptr, const char* secondNoteBody = nullptr) const {
    lastSelectedIndex = selectedIndex;
    lastFirstOptionIndex = firstOptionIndex;
    lastNoteLabel = noteLabel ? noteLabel : "";
    lastNoteBody = noteBody ? noteBody : "";
    lastSecondNoteLabel = secondNoteLabel ? secondNoteLabel : "";
    lastSecondNoteBody = secondNoteBody ? secondNoteBody : "";
  }

  int getLastSelectedIndex() const { return lastSelectedIndex; }
  int getLastFirstOptionIndex() const { return lastFirstOptionIndex; }

  mutable std::string lastNoteLabel;
  mutable std::string lastNoteBody;
  mutable std::string lastSecondNoteLabel;
  mutable std::string lastSecondNoteBody;

 private:
  mutable int lastSelectedIndex = -1;
  mutable int lastFirstOptionIndex = -1;
};

class UITheme {
 public:
  static UITheme& getInstance() {
    static UITheme instance;
    return instance;
  }

  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hints, bool) const {
    const auto& e = renderer.insets;
    return Rect{e[3], e[0], renderer.getScreenWidth() - e[3] - e[1],
                renderer.getScreenHeight() - e[0] - e[2] - (hints ? metrics.buttonHintsHeight : 0)};
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
