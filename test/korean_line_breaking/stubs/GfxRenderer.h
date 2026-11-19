#pragma once
#include <BidiUtils.h>
#include <EpdFontFamily.h>

#include <deque>
#include <string>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  uint64_t textCullGeneration() const { return 0; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 480; }
  int getDisplayHeight() const { return 480; }
  void beginTextBounds(int*) const {}
  void endTextBounds() const {}
  bool getCodepointMetrics(int, uint32_t, EpdFontFamily::Style, int32_t&, int&) const { return false; }
  int getFontIdForSize(int id, uint8_t) const { return id; }
  int resolveBookFont(int id, uint32_t, uint8_t size = 0) const { return getFontIdForSize(id, size); }
  struct TextVerticalBounds {
    int top;
    int bottom;
  };
  bool getLatinBodyBounds(int font, TextVerticalBounds& body, TextVerticalBounds& extent) const {
    body = extent = {0, getFontAscenderSize(font)};
    return true;
  }
  template <class... Args>
  TextVerticalBounds getTextVerticalBounds(int font, const char*, Args...) const {
    return {0, getFontAscenderSize(font)};
  }
  int getFontAscenderSize(int id) const { return 12 + id; }
  int getLineHeight(int id) const { return 16 + id; }
  static int characters(const char* text) {
    int n = 0;
    for (; *text; ++text)
      if ((static_cast<unsigned char>(*text) & 0xC0) != 0x80) ++n;
    return n;
  }
  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {
    return characters(text) * 8;
  }
  int getTextAdvanceX(int id, const char* text, EpdFontFamily::Style style, uint32_t = 0, int8_t = 0) const {
    return getTextWidth(id, text, style);
  }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isFontCacheScanning() const { return false; }
  template <class... Args>
  void drawLine(Args...) const {}
  template <class... Args>
  void drawText(Args...) const {}
  template <class... Args>
  void fillRect(Args...) const {}
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
