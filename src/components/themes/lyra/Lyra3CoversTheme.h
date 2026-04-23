

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.menuRowHeight = 46;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 3;
  v.keyboardKeyHeight = 50;
  v.keyboardCenteredText = true;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer,
                           const BookReadingStats* stats = nullptr, float progressPercent = -1.0f) const override;
};
