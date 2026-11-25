#pragma once

#include <cstdint>

class FontCacheManager;

class GfxRenderer {
 public:
  uint32_t publisherUsage = 0;
  uint32_t publisherSignature = 0;
  mutable uint32_t restoredPublisherUsage = 0;
  uint32_t bookFontUsage() const { return publisherUsage; }
  uint32_t bookFontLoaded() const { return publisherUsage; }
  uint32_t bookFontSignature(uint32_t) const { return publisherSignature; }
  bool restoreBookFonts(uint32_t mask, uint32_t loaded, uint32_t signature) const {
    restoredPublisherUsage = mask;
    return loaded == mask && signature == publisherSignature;
  }
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
};
