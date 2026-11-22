#pragma once

#include <algorithm>
#include <cstdint>

class SdCardFont {
 public:
  struct PrewarmCall {
    uint32_t codepoints[64] = {};
    uint16_t count = 0;
    uint8_t styleMask = 0;
  };

  void clearCache() {}
  void releaseForLowMemory(bool preserveAdvanceTable) {
    releaseForLowMemoryCallCount++;
    lastPreserveAdvanceTable = preserveAdvanceTable;
  }
  int prewarm(const char* text, uint8_t styleMask, bool, bool) {
    (void)text;
    return prewarmCodepoints(nullptr, 0, styleMask, false, true);
  }
  int prewarmCodepoints(const uint32_t* codepoints, uint32_t count, uint8_t styleMask, bool, bool) {
    auto& call = prewarmCalls[prewarmCallCount++];
    call.count = static_cast<uint16_t>(count < 64 ? count : 64);
    if (codepoints && call.count) std::copy_n(codepoints, call.count, call.codepoints);
    call.styleMask = styleMask;
    return 0;
  }
  uint8_t resolveStyle(uint8_t style) const { return resolvedStyles[style & 0x03]; }
  bool lastPrewarmFailed() const { return false; }
  void logStats(const char*) {}
  void resetStats() {}

  PrewarmCall prewarmCalls[4] = {};
  int prewarmCallCount = 0;
  int releaseForLowMemoryCallCount = 0;
  bool lastPreserveAdvanceTable = true;
  uint8_t resolvedStyles[4] = {0, 1, 2, 3};
};
