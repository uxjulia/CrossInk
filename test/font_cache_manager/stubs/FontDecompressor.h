#pragma once

#include <algorithm>
#include <cstdint>

#include "EpdFontFamily.h"

class FontDecompressor {
 public:
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;
  struct PrewarmCall {
    const EpdFontData* fontData = nullptr;
    uint32_t codepoints[64] = {};
    uint16_t count = 0;
  };

  void clearCache() { clearCacheCallCount++; }
  int prewarmCache(const EpdFontData* fontData, const char* text) {
    (void)text;
    return prewarmCodepoints(fontData, nullptr, 0);
  }
  int prewarmCodepoints(const EpdFontData* fontData, const uint32_t* codepoints, uint16_t count) {
    auto& call = prewarmCalls[prewarmCallCount++];
    call.fontData = fontData;
    call.count = count < 64 ? count : 64;
    if (codepoints && call.count) std::copy_n(codepoints, call.count, call.codepoints);
    return 0;
  }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[16] = {};
  int prewarmCallCount = 0;
  int clearCacheCallCount = 0;
};
