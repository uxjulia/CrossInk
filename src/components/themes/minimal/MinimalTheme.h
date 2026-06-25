#pragma once

#include <cstdint>

#include "components/themes/lyra/LyraTheme.h"

namespace MinimalMetrics {
constexpr int coverWidthForHeight(const int coverHeight) {
  return static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
}

constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 583;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverWidth = coverWidthForHeight(values.homeCoverHeight);
constexpr int homeCoverImageWidth = homeCoverWidth;
constexpr int homeCoverImageHeight = 525;
}  // namespace MinimalMetrics

struct GlobalReadingStats;

class MinimalTheme : public LyraTheme {
 public:
  static void setHomeButtonHintSelection(int selectedIndex);
  static int compactFileBrowserRowHeightFor(const GfxRenderer& renderer);
  static void drawCompactFileBrowserList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                         const std::function<std::string(int index)>& rowTitle,
                                         const std::function<std::string(int index)>& rowSubtitle,
                                         const std::function<UIIcon(int index)>& rowIcon,
                                         const std::function<std::string(int index)>& rowValue,
                                         const std::function<bool(int index)>& rowDimmed = nullptr);

  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool readerContext = false) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& isHeader = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                       bool allowInvertedText = false) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats = nullptr,
                       float progressPercent = -1.0f) const;
  void drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                            const GlobalReadingStats* globalStats, float progressPercent = -1.0f) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<const char*(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool usesCompactFileBrowserRows() const override { return true; }
  int compactFileBrowserRowHeight(const GfxRenderer& renderer) const override;
};
