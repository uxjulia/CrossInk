#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  // Bitmask of text style flags carried per-word through layout and serialized in page cache.
  // Bits 0-1 select the font variant; higher bits are render-time overlays.
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32,
    SMALL_CAPS = 64,
    RUBY_CONTINUE = 128,
  };
  struct GlyphData {
    const EpdFontData* fontData;
    const EpdGlyph* glyph;
  };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr, const EpdFont* fallback = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic), fallback(fallback) {}
  ~EpdFontFamily() = default;
  // Bitmap size identity belongs to the registration, leaving generated font data unchanged.
  EpdFontFamily withSize(int familyId, uint8_t points) const {
    auto family = *this;
    family.sizeFamilyId_ = familyId;
    family.pointSize_ = points;
    return family;
  }
  int sizeFamilyId() const { return sizeFamilyId_; }
  uint8_t pointSize() const { return pointSize_; }
  // Missing glyphs retain their own font data and share the primary baseline.
  EpdFontFamily withFallbackFonts(const EpdFont* regularFont, const EpdFont* boldFont = nullptr) const {
    auto family = *this;
    family.fallbackRegular = regularFont;
    family.fallbackBold = boldFont;
    return family;
  }
  // Optional ink bounds use font coordinates, increasing upward from the baseline.
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR, int* inkMinY = nullptr,
                         int* inkMaxY = nullptr) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  GlyphData findGlyphData(uint32_t cp, Style style = REGULAR) const;
  GlyphData getGlyphData(uint32_t cp, Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  // Blank spacing metrics, without replacement-glyph lookup or bitmap allocation.
  uint16_t getSpaceAdvanceX(uint32_t cp, Style style = REGULAR) const;
  uint32_t getFallbackCodepoint(uint32_t cp, Style style = REGULAR) const;
  /// Returns true if the resolved style or shared fallback can render `cp`
  /// (interval coverage only — see EpdFont::hasCodepoint).
  bool hasCodepoint(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;

 private:
  int sizeFamilyId_ = 0;
  uint8_t pointSize_ = 0;
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
  // Optional shared glyphs keep their own bitmap size across font sizes/styles.
  const EpdFont* fallback;

  const EpdFont* fallbackRegular = nullptr;
  const EpdFont* fallbackBold = nullptr;
  const EpdFont* getFallbackFont(Style style) const {
    return (style & BOLD) && fallbackBold ? fallbackBold : fallbackRegular;
  }
  const EpdFont* getFont(Style style) const;
};
