#pragma once

#include <cstdint>

class GfxRenderer;

struct Rect {
  int x;
  int y;
  int width;
  int height;
  Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct ThemeMetrics {
  int buttonHintsHeight = 0;
  int scrollBarWidth = 0;
  int scrollBarRightOffset = 0;
  int optionPopupItemSpacing = 0;
  int optionPopupInnerPadding = 0;
  int optionPopupSelectionHPadding = 0;
  int optionPopupSelectionVPadding = 0;
  int optionPopupTitleGap = 0;
  bool optionPopupOptionFontBold = false;
  bool optionPopupSelectionLight = false;
  int optionPopupDialogSideMargin = 0;
};
