#pragma once

#include <cstdint>

struct EpdGlyph {
  uint8_t width = 1, height = 1;
  uint32_t dataOffset = 0;
};
struct EpdFontData {
  const void* groups = nullptr;
  void* glyphMissCtx = nullptr;
  const uint8_t* (*bitmapHandler)(void*, const EpdGlyph*) = nullptr;
  const EpdGlyph* (*dynamicGlyphHandler)(void*, uint32_t) = nullptr;
};

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, SMALL_CAPS = 4 };

  EpdFontFamily() = default;
  EpdFontFamily(const EpdFontData* regular, const EpdFontData* bold, const EpdFontData* italic,
                const EpdFontData* boldItalic)
      : styleData{regular, bold, italic, boldItalic} {}

  const EpdFontData* getData(Style style) const {
    const uint8_t requested = static_cast<uint8_t>(style) & 0x03;
    if (styleData[requested]) return styleData[requested];
    if ((requested & BOLD) && styleData[BOLD]) return styleData[BOLD];
    if ((requested & ITALIC) && styleData[ITALIC]) return styleData[ITALIC];
    return styleData[REGULAR];
  }

  struct GlyphData {
    const EpdGlyph* glyph;
    const EpdFontData* fontData;
  };
  GlyphData getGlyphData(EpdGlyph&, uint32_t cp, Style style) const {
    const auto* data = getData(style);
    return {data && data->dynamicGlyphHandler ? data->dynamicGlyphHandler(data->glyphMissCtx, cp) : nullptr, data};
  }
  bool hasCodepoint(uint32_t, Style) const { return true; }
  uint32_t applyLigatures(uint32_t cp, const char*&, Style) const { return cp; }

 private:
  const EpdFontData* styleData[4] = {};
};
