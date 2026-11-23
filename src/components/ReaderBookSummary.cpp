#include "ReaderBookSummary.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "UITheme.h"
#include "fontIds.h"

namespace {
// Space between the header rule and the chapter line's glyph box.
constexpr int kChapterTopGap = 4;

bool hasChapter(const char* chapter) { return chapter != nullptr && chapter[0] != '\0'; }

int chapterLineHeight(const GfxRenderer& renderer, const char* chapter) {
  return hasChapter(chapter) ? kChapterTopGap + renderer.getLineHeight(renderer.filenameFontId(UI_10_FONT_ID)) : 0;
}
int progressWidth(const int width) {
  return std::max(0, width - UITheme::getInstance().getMetrics().contentSidePadding * 2);
}

// Prefer normal size, then the original compact UI font, then the smallest
// built-in font. A long localized line moves only the estimates to a second
// row, rather than clipping the last estimate.
int timeLeftFont(const GfxRenderer& renderer, const char* text, const int width) {
  for (const int fontId : {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID}) {
    if (renderer.getTextWidth(fontId, text) <= progressWidth(width)) return fontId;
  }
  return 0;
}
}  // namespace

namespace ReaderBookSummary {

int height(const GfxRenderer& renderer, const char* chapter, const char* progress, const ReaderTimeLeftLabels& timeLeft,
           const int width) {
  int height = chapterLineHeight(renderer, chapter) + UITheme::getInstance().getMetrics().tabBarHeight;
  if (!timeLeft.empty()) {
    char line[192];
    std::snprintf(line, sizeof(line), "%s", progress);
    appendReaderTimeLeft(line, sizeof(line), timeLeft);
    if (!timeLeftFont(renderer, line, width)) height += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }
  return height;
}

void draw(const GfxRenderer& renderer, const Rect& rect, const char* chapter, const char* progress,
          const ReaderTimeLeftLabels& timeLeft) {
  const int chapterHeight = chapterLineHeight(renderer, chapter);
  if (chapterHeight > 0) {
    const int sidePadding = UITheme::getInstance().getMetrics().contentSidePadding;
    const std::string visible =
        renderer.truncatedText(renderer.filenameFontId(UI_10_FONT_ID), chapter, rect.width - sidePadding * 2);
    renderer.drawText(renderer.filenameFontId(UI_10_FONT_ID), rect.x + sidePadding, rect.y + kChapterTopGap,
                      visible.c_str());
  }
  const Rect row{rect.x, rect.y + chapterHeight, rect.width, UITheme::getInstance().getMetrics().tabBarHeight};
  const int padding = UITheme::getInstance().getMetrics().contentSidePadding;
  int ruleY = row.y + row.height - 1;
  if (timeLeft.empty()) {
    GUI.drawSubHeader(renderer, row, progress);
  } else {
    char line[192];
    std::snprintf(line, sizeof(line), "%s", progress);
    appendReaderTimeLeft(line, sizeof(line), timeLeft);
    const int font = timeLeftFont(renderer, line, rect.width);
    if (font == UI_12_FONT_ID) {
      GUI.drawSubHeader(renderer, row, line);
    } else if (font != 0) {
      const int top = std::max(0, (row.height - renderer.getLineHeight(font)) / 2);
      renderer.drawText(font, row.x + padding, row.y + top, line);
    } else {
      // The themed subheader can draw its own bottom rule. These two lines
      // share one rule below the estimates, so draw the progress text directly.
      const int width = progressWidth(rect.width);
      const auto progressText = renderer.truncatedText(UI_10_FONT_ID, progress, width);
      const int top = std::max(0, (row.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2);
      renderer.drawText(UI_10_FONT_ID, row.x + padding, row.y + top, progressText.c_str());
      line[0] = '\0';
      appendReaderTimeLeft(line, sizeof(line), timeLeft);
      const int secondaryFont = renderer.getTextWidth(UI_10_FONT_ID, line) <= width ? UI_10_FONT_ID : SMALL_FONT_ID;
      const auto visible = renderer.truncatedText(secondaryFont, line, width);
      renderer.drawText(secondaryFont, row.x + padding, row.y + row.height, visible.c_str());
      ruleY += renderer.getLineHeight(UI_10_FONT_ID) + 4;
    }
  }
  renderer.drawLine(row.x, ruleY, row.x + row.width - 1, ruleY, 1, true);
}

}  // namespace ReaderBookSummary
