#include "EpdFontFamily.h"

#include <Utf8.h>

#include <algorithm>

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  if (*string == '\0') {
    *w = 0;
    *h = 0;
    return;
  }

  int lastBaseX = 0;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string, style);
      cp = getFallbackCodepoint(cp, style);
    }

    const bool hasRealGlyph = findGlyphData(cp, style).glyph != nullptr;

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::solidAdvanceX(data, getGlyph('M', style));
      const int glyphHeight = syntheticGlyph::solidHeight(data, cp);
      const int glyphWidth = syntheticGlyph::solidWidth(cp, advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::solidLeft(cp, advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::solidTop(data, cp, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::greekAdvanceX(data, getGlyph('M', style), cp);
      const int glyphHeight = syntheticGlyph::greekHeight(data, cp);
      const int glyphWidth = syntheticGlyph::greekWidth(cp, advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::greekLeft(cp, advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::greekTop(data, cp, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    if (!isCombining && !hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const EpdFontData* data = getData(style);
      const uint16_t advanceX = syntheticGlyph::replacementAdvanceX(data, getGlyph('M', style));
      const int glyphHeight = syntheticGlyph::replacementHeight(data);
      const int glyphWidth = syntheticGlyph::replacementWidth(advanceX, glyphHeight);
      const int glyphLeft = syntheticGlyph::replacementLeft(advanceX, glyphWidth);
      const int glyphTop = syntheticGlyph::replacementTop(data, glyphHeight);

      if (prevCp != 0) {
        const auto kernFP = getKerning(prevCp, cp, style);
        lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
      }

      minX = std::min(minX, lastBaseX + glyphLeft);
      maxX = std::max(maxX, lastBaseX + glyphLeft + glyphWidth);
      minY = std::min(minY, glyphTop - glyphHeight);
      maxY = std::max(maxY, glyphTop);

      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseTop = glyphTop;
      prevAdvanceFP = advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = getGlyph(cp, style);
    if (!glyph) {
      if (!isCombining) {
        lastBaseX += fp4::toPixel(prevAdvanceFP);
        prevCp = 0;
        prevAdvanceFP = 0;
        lastBaseLeft = 0;
        lastBaseWidth = 0;
        lastBaseTop = 0;
      }
      continue;
    }

    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(glyph->top, glyph->height, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      const auto kernFP = getKerning(prevCp, cp, style);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX =
        isCombining ? combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, glyph->left, glyph->width)
                    : lastBaseX;
    const int glyphBaseY = -raiseBy;

    minX = std::min(minX, glyphBaseX + glyph->left);
    maxX = std::max(maxX, glyphBaseX + glyph->left + glyph->width);
    minY = std::min(minY, glyphBaseY + glyph->top - glyph->height);
    maxY = std::max(maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseLeft = glyph->left;
      lastBaseWidth = glyph->width;
      lastBaseTop = glyph->top;
      prevAdvanceFP = glyph->advanceX;
      prevCp = cp;
    }
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

EpdFontFamily::GlyphData EpdFontFamily::findGlyphData(const uint32_t cp, const Style style) const {
  const EpdFont* font = getFont(style);
  if (const EpdGlyph* glyph = font->findGlyph(cp)) {
    return {font->data, glyph};
  }

  if (font != regular) {
    if (const EpdGlyph* glyph = regular->findGlyph(cp)) {
      return {regular->data, glyph};
    }
  }

  return {nullptr, nullptr};
}

EpdFontFamily::GlyphData EpdFontFamily::getGlyphData(const uint32_t cp, const Style style) const {
  if (const GlyphData glyphData = findGlyphData(cp, style); glyphData.glyph) {
    return glyphData;
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyphData(REPLACEMENT_GLYPH, style);
  }
  return {nullptr, nullptr};
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getGlyphData(cp, style).glyph;
}

uint32_t EpdFontFamily::getFallbackCodepoint(const uint32_t cp, const Style style) const {
  if (findGlyphData(cp, style).glyph) return cp;
  const uint32_t aliasCp = syntheticGlyph::aliasCodepoint(cp);
  if (aliasCp != cp) {
    return findGlyphData(aliasCp, style).glyph ? aliasCp : REPLACEMENT_GLYPH;
  }
  if (syntheticGlyph::isSpaceFallback(cp)) return cp;
  if (syntheticGlyph::isSolid(cp) || syntheticGlyph::isGreekFallback(cp)) return cp;
  return REPLACEMENT_GLYPH;
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
