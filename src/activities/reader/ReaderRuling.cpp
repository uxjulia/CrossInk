#include "ReaderRuling.h"

#include "ReaderUtils.h"

void ReaderRuling::drawSelectionPage(const GfxRenderer& renderer, const Page& page, int fontId, int left, int top) {
  if (!SETTINGS.ruledLines) return;
  const auto bezel = renderer.getViewableInsets().rotated(static_cast<unsigned>(renderer.getOrientation()));
  const int width = renderer.getScreenWidth() - left - SETTINGS.screenMarginHorizontal - bezel.edges[1];
  const int bottom =
      renderer.getScreenHeight() - bezel.edges[2] - ReaderUtils::getReaderFooterReservedHeight(renderer, false);
  ReaderRuling{SETTINGS.ruledLines, left, width, bottom, SETTINGS.lineHeightPercent}.drawPage(renderer, page, fontId,
                                                                                              top);
}
