#pragma once

#include <EpdFontFamily.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  inline static bool loanActive = false;
  inline static bool fileProbeHadLoan = false;
  uint32_t loans = 0;
  bool hasFrameBuffer() const { return !loanActive; }
  uint32_t frameBufferLoanCount() const { return loans; }
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer) : active_(!loanActive) {
      if (active_) {
        loanActive = true;
        ++renderer.loans;
      }
    }
    ~FrameBufferLoan() { end(); }
    void end() {
      if (active_) {
        loanActive = false;
        active_ = false;
      }
    }

   private:
    bool active_;
  };
  int textAdvancePerChar = 0;
  uint8_t scalableBaseSize = 0;
  uint8_t getFontPointSize(int id) const { return scalableBaseSize ? (id ? id : scalableBaseSize) : 0; }
  int getFontIdForSize(int id, uint8_t size) const { return scalableBaseSize && size ? size : id; }
  int resolveBookFont(int id, uint32_t, uint8_t size = 0) const { return getFontIdForSize(id, size); }
  int getFontAscenderSize(int id) const { return scalableBaseSize ? getFontPointSize(id) : 12; }
  bool getCodepointMetrics(int id, uint32_t, EpdFontFamily::Style, int32_t& advance, int& top) const {
    advance = std::max(1, textAdvancePerChar) * 16;
    top = getFontAscenderSize(id);
    return true;
  }
  int getLineHeight(int id) const { return scalableBaseSize ? getFontPointSize(id) * 2 : 16; }
  int getTextWidth(int, const char*, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 0; }
  int getTextAdvanceX(int id, const char* text, EpdFontFamily::Style, uint32_t = 0, int8_t = 0) const {
    return static_cast<int>(std::strlen(text)) * textAdvancePerChar * (scalableBaseSize ? getFontPointSize(id) : 1) /
           (scalableBaseSize ? scalableBaseSize : 1);
  }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 0; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  bool sdFont = false;
  mutable size_t prewarmCalls = 0;
  mutable std::vector<uint32_t> prewarmed;
  bool isSdCardFont(int) const { return sdFont; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
  bool ensureSdCardFontReady(int, const uint32_t* codepoints, size_t count, bool, bool, uint8_t) const {
    ++prewarmCalls;
    prewarmed.insert(prewarmed.end(), codepoints, codepoints + count);
    return true;
  }
  bool ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const { return true; }
  bool ensureSdCardFontReady(int, const char*, uint8_t) const { return true; }
  std::vector<std::string> wrappedText(int, const char*, int, int,
                                       EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return {};
  }
};
