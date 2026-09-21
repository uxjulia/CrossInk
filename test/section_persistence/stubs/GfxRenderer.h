#pragma once

#include <cstdint>

class FontCacheManager;

class GfxRenderer {
 public:
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
};
