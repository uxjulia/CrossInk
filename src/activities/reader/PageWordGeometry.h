#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <algorithm>

struct PageWordGeometry {
  int xOffset = 0;
  int width = 0;
};

inline PageWordGeometry clipPageTextRange(const PageTextLine& line, const int xOffset, const int width) {
  if (line.clipWidth <= 0) return {xOffset, width};
  const int visibleLeft = std::max(0, xOffset);
  const int visibleRight = std::min(line.clipWidth, xOffset + width);
  return {visibleLeft, std::max(0, visibleRight - visibleLeft)};
}

inline PageWordGeometry pageWordGeometry(const GfxRenderer& renderer, const int fontId, const PageTextLine& line,
                                         const TextBlock& block, const uint16_t wordIndex) {
  const auto style = static_cast<EpdFontFamily::Style>(block.wordStyle(wordIndex) & ~EpdFontFamily::UNDERLINE);
  const int lineFontId = block.wordFontId(renderer, fontId, wordIndex);
  int width =
      renderer.getTextAdvanceX(lineFontId, block.visibleWordText(wordIndex), style, 0, block.getCharacterSpacing());
  if (wordIndex + 1 < block.wordCount() && block.wordXpos(wordIndex + 1) > block.wordXpos(wordIndex)) {
    width = std::min(width, static_cast<int>(block.wordXpos(wordIndex + 1) - block.wordXpos(wordIndex)));
  }
  return clipPageTextRange(line, block.wordXpos(wordIndex), width);
}
