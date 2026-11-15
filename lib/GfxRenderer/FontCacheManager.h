#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  enum class PreparationPolicy : uint8_t { Normal, DictionaryLean, IdleScalable };

  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  void releaseSdFontCaches();
  // Call only between render/layout operations with the render lock held.
  // Keep all caches of the active SD font (including aliases) warm for layout.
  void releaseInactiveSdFontCaches(int activeFontId);
  bool prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F,
                    PreparationPolicy policy = PreparationPolicy::Normal);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

#if CROSSINK_SCALABLE_FONTS
  // Fixed scratch owned by the reader, not the stack. No page/font pointers survive a tick.
  struct IdleScalableJob {
    uint32_t codepoints[512] = {};
    int fontIds[4] = {};
    uint16_t count = 0;
    uint16_t cursor = 0;
    void reset() { count = cursor = 0; }
  };
  enum class IdleResult : uint8_t { Complete, Yielded, Failed };
  bool isScalableFont(int fontId) const;
  IdleResult prewarmIdleSlice(IdleScalableJob& job, bool (*shouldYield)(void*), void* context, uint16_t maxGlyphs = 16,
                              bool (*cancelled)(void*) = nullptr);
#endif

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager, PreparationPolicy policy);
    ~PrewarmScope();
    bool endScanAndPrewarm();
#if CROSSINK_SCALABLE_FONTS
    void endScanForIdle(IdleScalableJob& job);
#endif
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    PreparationPolicy policy_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope(PreparationPolicy policy = PreparationPolicy::Normal);

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // A render pass touches at most a handful of font ids. Codepoints are packed
  // with a compact font slot and resolved style, then grouped for prewarming.
  static constexpr uint8_t MAX_SCAN_FONTS = 4;
  static constexpr uint16_t MAX_SCAN_CODEPOINTS = 512;
  static constexpr uint8_t SCAN_STYLE_SHIFT = 21;
  static constexpr uint8_t SCAN_FONT_SHIFT = SCAN_STYLE_SHIFT + 2;
  static constexpr uint32_t SCAN_CODEPOINT_MASK = (1U << SCAN_STYLE_SHIFT) - 1;
  static constexpr uint8_t SCAN_GROUP_COUNT = MAX_SCAN_FONTS * 4;

  uint8_t resolveScanStyle(int fontId, EpdFontFamily::Style style) const;
  int scanFontIds_[MAX_SCAN_FONTS] = {};
  uint32_t scanCodepoints_[MAX_SCAN_CODEPOINTS + 1] = {};
  uint16_t scanGroupCounts_[SCAN_GROUP_COUNT] = {};
  uint16_t scanCodepointCount_ = 0;
  uint8_t scanFontCount_ = 0;
  bool scanOverflowWarned_ = false;
};
