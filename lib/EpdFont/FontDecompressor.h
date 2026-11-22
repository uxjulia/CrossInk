#pragma once

#include <InflateReader.h>

#include <memory>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page slots (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page slots + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page slot.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  // Prewarm a sorted, unique codepoint span collected by the reader's scan pass.
  // Returns the number of glyphs that could not be cached, or -1 if no page
  // slot is available and rendering should use the hot-group path.
  int prewarmCodepoints(const EpdFontData* fontData, uint32_t* codepoints, uint16_t count);

  struct Stats {
    uint32_t prewarmSlotRefusals = 0;  // Explicit fallback, not a successful page-slot admission.
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;     // page slot buffer allocations
    uint32_t pageGlyphsBytes = 0;     // page slot lookup table allocations
    uint32_t hotGroupBytes = 0;       // current hot group allocation
    uint32_t peakTempBytes = 0;       // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;     // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;      // number of getBitmap calls
    uint32_t hotOffsetCacheHits = 0;  // repeated glyphs avoid rescanning earlier group members
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffers: flat arrays of prewarmed glyph bitmaps with sorted lookup
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer;
    const EpdFontData* fontData;
    PageGlyphEntry* glyphs;
    uint16_t glyphCount;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  // Nothrow high-water malloc buffers, NOT std::vector: getBitmap() runs on the render path,
  // and under -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of
  // failing (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
  // ensureCapacity() returns false on OOM so the caller can skip the glyph gracefully.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroup = nullptr;  // owned; freed in freeHotGroup()/dtor
  uint32_t hotGroupCapacity = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  static constexpr uint8_t HOT_OFFSET_CACHE_SIZE = 16;
  struct HotOffsetEntry {
    uint32_t glyphIndex = UINT32_MAX;
    uint32_t alignedOffset = 0;
  };
  HotOffsetEntry hotOffsetCache_[HOT_OFFSET_CACHE_SIZE] = {};

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeHotGroup();
  void resetHotOffsetCache();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
  void addLigatureGlyphs(const EpdFontData* fontData, uint32_t* neededGlyphs, uint16_t& glyphCount,
                         bool& glyphCapWarned);
  uint16_t countLigatureOutputs(const EpdFontData* fontData, const uint32_t* glyphIndices, uint16_t glyphCount);
  void addLigatureGlyphEntries(const EpdFontData* fontData, PageGlyphEntry* glyphs, uint16_t& glyphCount,
                               uint16_t glyphCapacity);
  int prewarmGlyphs(const EpdFontData* fontData, uint32_t* neededGlyphs, uint16_t glyphCount);
  int prewarmGlyphTable(const EpdFontData* fontData, std::unique_ptr<PageGlyphEntry[]> glyphs, uint16_t glyphCount);
};
