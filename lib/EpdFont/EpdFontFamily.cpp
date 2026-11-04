#include "EpdFontFamily.h"

#include <Utf8.h>

#include <algorithm>

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font variant bits; decoration and positioning bits do not affect font selection.
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

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style, int* inkMinY,
                                      int* inkMaxY) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  int boundsMinY = 0, boundsMaxY = 0;
  bool hasInk = false;
  const auto includeVerticalBounds = [&](const int bottom, const int top) {
    minY = std::min(minY, bottom);
    maxY = std::max(maxY, top);
    if ((!inkMinY && !inkMaxY) || top <= bottom) return;
    boundsMinY = hasInk ? std::min(boundsMinY, bottom) : bottom;
    boundsMaxY = hasInk ? std::max(boundsMaxY, top) : top;
    hasInk = true;
  };

  if (*string == '\0') {
    *w = 0;
    *h = 0;
    if (inkMinY) *inkMinY = 0;
    if (inkMaxY) *inkMaxY = 0;
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
    if (utf8IsVariationSelector(cp)) continue;

    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string, style);
      cp = getFallbackCodepoint(cp, style);
    }

    const bool hasRealGlyph = findGlyphData(cp, style).glyph != nullptr;

    if (!isCombining && syntheticGlyph::isBlankSpace(cp, hasRealGlyph)) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);
      prevCp = cp;
      prevAdvanceFP = getSpaceAdvanceX(cp, style);
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
      includeVerticalBounds(glyphTop - glyphHeight, glyphTop);

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
      includeVerticalBounds(glyphTop - glyphHeight, glyphTop);

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
      includeVerticalBounds(glyphTop - glyphHeight, glyphTop);

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

    const auto anchor = combiningMark::anchorFor(cp);
    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(anchor, glyph->top, glyph->height, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      const auto kernFP = getKerning(prevCp, cp, style);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX = isCombining ? combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                                   glyph->left, glyph->width)
                                       : lastBaseX;
    const int glyphBaseY = raiseBy;

    minX = std::min(minX, glyphBaseX + glyph->left);
    maxX = std::max(maxX, glyphBaseX + glyph->left + glyph->width);
    includeVerticalBounds(glyphBaseY + glyph->top - glyph->height, glyphBaseY + glyph->top);

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
  if (inkMinY) *inkMinY = boundsMinY;
  if (inkMaxY) *inkMaxY = boundsMaxY;
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

  if (fallback) {
    if (const EpdGlyph* glyph = fallback->findGlyph(cp)) {
      return {fallback->data, glyph};
    }
  }
  const auto extra = getFallbackFont(style);
  if (extra) {
    if (const auto glyph = extra->findGlyph(cp)) return {extra->data, glyph};
    if (extra != fallbackRegular && fallbackRegular) {
      if (const auto glyph = fallbackRegular->findGlyph(cp)) return {fallbackRegular->data, glyph};
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

uint16_t EpdFontFamily::getSpaceAdvanceX(const uint32_t cp, const Style style) const {
  if (syntheticGlyph::isZeroWidthSpace(cp)) return 0;
  const auto native = findGlyphData(cp, style).glyph;
  if (native && native->advanceX > 0) return native->advanceX;

  // Copy each metric before the next lookup: dynamic fonts can reuse glyph slots.
  const auto advance = [&](uint32_t codepoint) -> uint16_t {
    const auto glyph = findGlyphData(codepoint, style).glyph;
    return glyph ? glyph->advanceX : 0;
  };
  const uint16_t space = advance(' ');
  uint16_t em = advance('M');
  if (!em) em = syntheticGlyph::solidAdvanceX(getData(style), nullptr);
  const uint16_t figure = cp == 0x2007 ? advance('0') : 0;
  const uint16_t punctuation = cp == 0x2008 ? advance('.') : 0;
  return syntheticGlyph::spaceAdvanceX(cp, em, space, figure, punctuation);
}

uint32_t EpdFontFamily::getFallbackCodepoint(const uint32_t cp, const Style style) const {
  if (syntheticGlyph::isSpaceFallback(cp)) return cp;
  if (findGlyphData(cp, style).glyph) return cp;
  const uint32_t aliasCp = syntheticGlyph::aliasCodepoint(cp);
  if (aliasCp != cp) {
    return findGlyphData(aliasCp, style).glyph ? aliasCp : REPLACEMENT_GLYPH;
  }
  if (syntheticGlyph::isSolid(cp) || syntheticGlyph::isGreekFallback(cp)) return cp;
  return REPLACEMENT_GLYPH;
}

bool EpdFontFamily::hasCodepoint(const uint32_t cp, const Style style) const {
  const auto extra = getFallbackFont(style);
  return getFont(style)->hasCodepoint(cp) || regular->hasCodepoint(cp) || (fallback && fallback->hasCodepoint(cp)) ||
         (extra && extra->hasCodepoint(cp)) || (fallbackRegular && fallbackRegular->hasCodepoint(cp));
}

int8_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  if (syntheticGlyph::isSpaceFallback(leftCp) || syntheticGlyph::isSpaceFallback(rightCp)) return 0;
  const auto primary = getFont(style);
  if (!fallbackRegular) return primary->getKerning(leftCp, rightCp);
  // Never apply a kerning pair across different faces in a mixed-script filename.
  const auto owner = [&](uint32_t cp) -> const EpdFont* {
    if (primary->hasCodepoint(cp)) return primary;
    if (regular->hasCodepoint(cp)) return regular;
    if (fallback && fallback->hasCodepoint(cp)) return fallback;
    const auto extra = getFallbackFont(style);
    if (extra && extra->hasCodepoint(cp)) return extra;
    return fallbackRegular->hasCodepoint(cp) ? fallbackRegular : nullptr;
  };
  const auto leftFont = owner(leftCp);
  return leftFont && leftFont == owner(rightCp) ? leftFont->getKerning(leftCp, rightCp) : 0;
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  return getFont(style)->applyLigatures(cp, text);
}
