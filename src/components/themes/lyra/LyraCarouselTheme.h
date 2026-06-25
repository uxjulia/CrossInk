

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra Carousel theme metrics (zero runtime cost)
namespace LyraCarouselMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  v.listRowHeight = 35;
  v.menuRowHeight = 64;
  v.menuSpacing = 8;
  v.homeTopPadding = 28;
  v.homeCoverHeight = 600;
  v.homeCoverTileHeight = 660;
  v.homeRecentBooksCount = 3;
  v.keyboardKeyHeight = 50;
  v.keyboardCenteredText = true;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace LyraCarouselMetrics

class LyraCarouselTheme : public LyraTheme {
 public:
  // Max cache geometry for the carousel artwork area.
  static constexpr int kCenterCoverW = 340;
  static constexpr int kCenterCoverH = LyraCarouselMetrics::values.homeCoverHeight - 60;  // 540
  static constexpr int kCenterCoverVisualInset = 10;
  static constexpr int kBaseDisplayCenterW = (kCenterCoverW * 86) / 100;
  static constexpr int kBaseDisplayCenterH = (kCenterCoverH * 86) / 100;
  static constexpr int kDisplayCenterW =
      ((kBaseDisplayCenterW + 24) < kCenterCoverW) ? (kBaseDisplayCenterW + 24) : kCenterCoverW;
  static constexpr int kDisplayCenterH =
      ((kBaseDisplayCenterH + 24) < kCenterCoverH) ? (kBaseDisplayCenterH + 24) : kCenterCoverH;
  // Actual center image rect after the visual inset; using this for the cached
  // thumbnail avoids scaling an already-dithered BMP at draw time.
  static constexpr int kCenterThumbW = kDisplayCenterW - kCenterCoverVisualInset * 2;  // 296
  static constexpr int kCenterThumbH = kDisplayCenterH - kCenterCoverVisualInset * 2;  // 468
  static constexpr int kSideCoverW = 200;
  static constexpr int kSideCoverH = LyraCarouselMetrics::values.homeCoverHeight - 210;  // 390

  static void setPreRenderIndex(int idx);
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<const char*(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  // LyraTheme has no virtual overlay hook; this is a carousel-only helper.
  void drawButtonMenuSelectionOverlay(const GfxRenderer& renderer, int buttonCount, int selectedIndex,
                                      const std::function<const char*(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const;
  void drawCarouselBorder(GfxRenderer& renderer, Rect coverRect, const std::vector<RecentBook>& recentBooks,
                          int centerIdx, bool inCarouselRow) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = {},
                const std::function<UIIcon(int index)>& rowIcon = {},
                const std::function<std::string(int index)>& rowValue = {}, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = {},
                const std::function<bool(int index)>& isHeader = {}) const override;
};
