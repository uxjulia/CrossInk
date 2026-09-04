#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <string>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  int getFontAscenderSize(int) const { return 12; }
  int getLineHeight(int) const { return 16; }
  int getTextWidth(int, const char*, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 0; }
  int getTextAdvanceX(int, const char*, EpdFontFamily::Style, uint32_t = 0) const { return 0; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 0; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
  bool ensureSdCardFontReady(int, const uint32_t*, size_t, bool, bool, uint8_t) const { return true; }
  bool ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const { return true; }
  bool ensureSdCardFontReady(int, const char*, uint8_t) const { return true; }
  std::vector<std::string> wrappedText(int, const char*, int, int,
                                       EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return {};
  }
};
