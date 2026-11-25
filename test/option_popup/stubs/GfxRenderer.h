#pragma once

#include <array>
#include <string>
#include <vector>

#include "EpdFontFamily.h"

class GfxRenderer {
 public:
  std::array<int, 4> insets{};
  int getScreenWidth() const { return 800; }
  int getScreenHeight() const { return 480; }
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(std::string(text).size()) * 8;
  }
  int getSpaceWidth(int, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 8; }
  std::vector<std::string> wrappedText(int, const char* text, int, int maxLines, EpdFontFamily::Style) const {
    if (maxLines <= 0 || text == nullptr || text[0] == '\0') return {};
    return {text};
  }
  void displayBuffer() const {}
  void drawLine(int, int, int, int) const { ++drawnLines; }
  void fillPolygon(const int*, const int*, int, bool) const { ++drawnTriangles; }
  int lineCount() const { return drawnLines; }
  int triangleCount() const { return drawnTriangles; }

 private:
  mutable int drawnLines = 0;
  mutable int drawnTriangles = 0;
};
