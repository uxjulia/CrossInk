#pragma once

#include <EpdFontFamily.h>

class GfxRenderer {
 public:
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char*, EpdFontFamily::Style) const { return 0; }
  void fillRect(int, int, int, int, bool) const {}
  void drawRect(int, int, int, int, bool) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style) const {}
};
