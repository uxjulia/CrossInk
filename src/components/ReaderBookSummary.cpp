#include "ReaderBookSummary.h"

#include <GfxRenderer.h>

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
}  // namespace

namespace ReaderBookSummary {

int height(const GfxRenderer& renderer, const char* chapter) {
  return chapterLineHeight(renderer, chapter) + UITheme::getInstance().getMetrics().tabBarHeight;
}

void draw(const GfxRenderer& renderer, const Rect& rect, const char* chapter, const char* progress) {
  const int chapterHeight = chapterLineHeight(renderer, chapter);
  if (chapterHeight > 0) {
    const int sidePadding = UITheme::getInstance().getMetrics().contentSidePadding;
    const std::string visible =
        renderer.truncatedText(renderer.filenameFontId(UI_10_FONT_ID), chapter, rect.width - sidePadding * 2);
    renderer.drawText(renderer.filenameFontId(UI_10_FONT_ID), rect.x + sidePadding, rect.y + kChapterTopGap,
                      visible.c_str());
  }
  const Rect row{rect.x, rect.y + chapterHeight, rect.width, UITheme::getInstance().getMetrics().tabBarHeight};
  GUI.drawSubHeader(renderer, row, progress);
  renderer.drawLine(row.x, row.y + row.height - 1, row.x + row.width - 1, row.y + row.height - 1, 1, true);
}

}  // namespace ReaderBookSummary
