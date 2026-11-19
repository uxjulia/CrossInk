#pragma once

#include <Epub/Page.h>
#include <Epub/RuledLineLayout.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

// Notebook rules are part of the monochrome page. Varying dot spacing keeps
// the three shades available on every display and with text AA turned off.
struct ReaderRuling {
  static constexpr int padding = RuledLineLayout::clearance;

  static RuledLineLayout textRow(const GfxRenderer& renderer, int fontId, const char* text, int requested,
                                 int8_t spacing) {
    const auto ink =
        renderer.getTextVerticalBounds(fontId, text, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, spacing);
    RuledLineLayout::Ink rowInk{ink.top, ink.bottom};
    rowInk.observeText(text);
    return RuledLineLayout::center(renderer, fontId, requested, rowInk);
  }

  uint8_t level = 0;
  int left = 0;
  int width = 0;
  int bottom = 0;  // Exclusive, to keep the footer clear.
  uint8_t lineHeightPercent = 100;

  static void drawSelectionPage(const GfxRenderer& renderer, const Page& page, int fontId, int left, int top);

  bool enabled(const GfxRenderer& renderer) const {
    return level >= 1 && level <= 3 && renderer.getRenderMode() == GfxRenderer::BW && !renderer.isFontCacheScanning();
  }

  void draw(const GfxRenderer& renderer, int rowBottom) const {
    if (rowBottom <= 0 || rowBottom > bottom) return;
    const int y = rowBottom - 1;  // Row bounds are exclusive.
    const int step = 1 << (3 - level);
    const int right = left + width;
    // Descenders and accents can extend outside their font's nominal row.
    // Leave a small break around existing ink, keeping the dot pattern aligned.
    int nextDot = left;
    const auto fillUntil = [&](int end) {
      for (; nextDot < end; nextDot += step) renderer.drawPixel(nextDot, y);
    };
    for (int x = left - padding; x < right + padding; ++x) {
      for (int dy = -padding; dy <= padding; ++dy) {
        if (!renderer.isPixelBlack(x, y + dy)) continue;
        fillUntil(x - padding);
        const int firstClear = x + padding + 1;
        nextDot = left + ((firstClear - left + step - 1) / step) * step;
        break;
      }
    }
    fillUntil(right);
  }

  void drawBlock(const GfxRenderer& renderer, const TextBlock& block, int fontId, int top) const {
    if (!block.wordCount()) return;
    // EPUB stores an explicit height when a row differs from the regular text layout.
    int height = block.getBlockStyle().lineHeight;
    if (!height) {
      const float compression = lineHeightPercent / 100.0f;
      height = std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * compression + 0.5f));
    }
    draw(renderer, top - block.getBlockStyle().ruledTextOffset + height);
  }

  void drawPage(const GfxRenderer& renderer, const Page& page, int fontId, int top) const {
    if (!enabled(renderer)) return;
    for (const auto& element : page.elements) {
      if (!element || element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock()) drawBlock(renderer, *line.getBlock(), fontId, top + line.yPos);
    }
  }
};
