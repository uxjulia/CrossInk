#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>

struct WordRef {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int pageIdx = 0;
  uint16_t pageWordIndex = 0;
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;
  bool endsWithInsertedHyphen = false;
  bool lineIsRtl = false;
};
