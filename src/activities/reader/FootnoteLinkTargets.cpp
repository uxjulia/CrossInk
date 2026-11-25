#include "FootnoteLinkTargets.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <algorithm>

FootnoteLinkTargets buildFootnoteLinkTargets(const Page& page, const std::vector<FootnoteEntry>& footnotes,
                                             const GfxRenderer& renderer, const int fontId, const int marginTop,
                                             const int marginLeft) {
  FootnoteLinkTargets targets{};
  if (footnotes.empty()) return targets;

  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;

    const auto& block = *line.getBlock();
    const int lineFontId = block.resolvedFontId(renderer, fontId);
    const int lineHeight =
        block.getBlockStyle().lineHeight ? block.getBlockStyle().lineHeight : block.maxLineHeight(renderer, lineFontId);
    for (uint16_t wordIndex = 0; wordIndex < block.wordCount(); ++wordIndex) {
      const uint8_t linkId = block.wordLinkId(wordIndex);
      if (linkId == 0) continue;

      const auto footnoteIt = std::find_if(footnotes.begin(), footnotes.end(),
                                           [linkId](const FootnoteEntry& entry) { return entry.linkId == linkId; });
      if (footnoteIt == footnotes.end()) continue;
      const size_t footnoteIndex = static_cast<size_t>(footnoteIt - footnotes.begin());
      if (footnoteIndex >= targets.size()) continue;

      const auto style = static_cast<EpdFontFamily::Style>(block.wordStyle(wordIndex) & ~EpdFontFamily::UNDERLINE);
      const int wordX = marginLeft + line.xPos + block.wordXpos(wordIndex);
      const int wordY = marginTop + line.yPos + block.wordYOffset(renderer, lineFontId, wordIndex);
      const int wordFontId = block.wordFontId(renderer, lineFontId, wordIndex);
      int wordWidth =
          renderer.getTextAdvanceX(wordFontId, block.visibleWordText(wordIndex), style, 0, block.getCharacterSpacing());
      if (wordIndex + 1 < block.wordCount() && block.wordXpos(wordIndex + 1) > block.wordXpos(wordIndex)) {
        wordWidth = std::min(wordWidth, static_cast<int>(block.wordXpos(wordIndex + 1) - block.wordXpos(wordIndex)));
      }
      if (wordWidth <= 0) continue;

      auto& target = targets[footnoteIndex];
      if (target.width <= 0 || target.height <= 0) {
        target = {static_cast<int16_t>(wordX), static_cast<int16_t>(wordY), static_cast<int16_t>(wordWidth),
                  static_cast<int16_t>(lineHeight)};
        continue;
      }

      const int left = std::min<int>(target.x, wordX);
      const int top = std::min<int>(target.y, wordY);
      const int right = std::max<int>(target.x + target.width, wordX + wordWidth);
      const int bottom = std::max<int>(target.y + target.height, wordY + lineHeight);
      target = {static_cast<int16_t>(left), static_cast<int16_t>(top), static_cast<int16_t>(right - left),
                static_cast<int16_t>(bottom - top)};
    }
  }
  return targets;
}
