#include "LyraCarouselTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
// Cover layout — keep Lyra Carousel's general geometry, but render the books
// with the same visual treatment as Lyra Flow.
constexpr int kCenterCoverMaxW = LyraCarouselTheme::kCenterCoverW;
constexpr int kCenterCoverMaxH = LyraCarouselTheme::kCenterCoverH;
constexpr int kCenterThumbW = LyraCarouselTheme::kCenterThumbW;
constexpr int kCenterThumbH = LyraCarouselTheme::kCenterThumbH;
constexpr int kSideCoverMaxW = LyraCarouselTheme::kSideCoverW;
constexpr int kSideCoverMaxH = LyraCarouselTheme::kSideCoverH;
constexpr int kCoverTopPad = 18;
constexpr int kCenterCoverVisualInset = LyraCarouselTheme::kCenterCoverVisualInset;
constexpr int kCarouselVerticalLift = 8;
constexpr int kBaseDisplayCenterW = LyraCarouselTheme::kBaseDisplayCenterW;
constexpr int kBaseDisplayCenterH = LyraCarouselTheme::kBaseDisplayCenterH;
constexpr int kDisplayCenterW = LyraCarouselTheme::kDisplayCenterW;
constexpr int kDisplayCenterH = LyraCarouselTheme::kDisplayCenterH;
constexpr int kNearSideW = (kBaseDisplayCenterW * 26) / 100;
constexpr int kFarSideW = (kBaseDisplayCenterW * 21) / 100;
constexpr int kNearSideInnerH = (kBaseDisplayCenterH * 90) / 100;
constexpr int kNearSideOuterH = (kBaseDisplayCenterH * 82) / 100;
constexpr int kFarSideInnerH = (kBaseDisplayCenterH * 84) / 100;
constexpr int kFarSideOuterH = (kBaseDisplayCenterH * 74) / 100;
constexpr int kSideOutlineW = 2;
constexpr int kSideCornerRadius = 5;
constexpr int kCoverStackLift = 15;
constexpr int kCenterCoverTopInset = (((kCenterCoverMaxH - kDisplayCenterH) / 2) > kCoverStackLift)
                                         ? ((kCenterCoverMaxH - kDisplayCenterH) / 2) - kCoverStackLift
                                         : 0;

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kMenuLabelFontId = SMALL_FONT_ID;
constexpr int kDotSize = 8;  // px square dot
constexpr int kDotGap = 6;   // px between dots
constexpr int kTitleTopClearance = 4;
constexpr int kTitleDrawOffset = 5;
constexpr int kTitleBottomGap = 8;
constexpr int kMenuLabelTopGap = 3;
constexpr int kMenuLabelBottomGap = 4;
constexpr int kMenuRowDrop = 31;

constexpr int kFooterTopGap = 10;
constexpr int kFooterLabelToBarGap = 3;
constexpr int kFooterProgressBarHeight = 5;
constexpr int kFooterPercentTopGap = 2;

constexpr int kCornerRadius = 6;
constexpr int kThinOutlineW = 1;    // always-visible outline around centre cover
constexpr int kSelectionLineW = 3;  // thicker outline when centre cover is selected
constexpr int kCenterOutlineW = 4;  // white ring around centre cover

// Icon row — icons are 32×32 bitmaps; drawIcon does NOT scale
constexpr int kMenuIconSize = 32;  // must match actual bitmap dimensions
constexpr int kMenuIconPad = 14;   // symmetric vertical padding → tile height = 60
constexpr int kHighlightPad = 7;   // highlight padding around the selected icon
// Row is anchored to the bottom of the screen, just above button hints
constexpr int kButtonHintsH = LyraCarouselMetrics::values.buttonHintsHeight;

struct MenuLayoutMetrics {
  int tileH;
  int tileW;
  int labelLineHeight;
  int rowY;
  int labelY;
};

MenuLayoutMetrics computeMenuLayout(const GfxRenderer& renderer, int buttonCount) {
  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  const int labelLineHeight = renderer.getLineHeight(kMenuLabelFontId);
  const int rowY = renderer.getScreenHeight() - kButtonHintsH - tileH - kMenuLabelTopGap - labelLineHeight -
                   kMenuLabelBottomGap + kMenuRowDrop;
  return {
      tileH, renderer.getScreenWidth() / buttonCount, labelLineHeight, rowY, rowY - kMenuLabelTopGap - labelLineHeight,
  };
}

std::atomic<int> lastCarouselSelectorIndex{-1};
Rect lastCenterCoverRect{0, 0, 0, 0};
Rect cachedCenterCoverRects[LyraCarouselMetrics::values.homeRecentBooksCount];

Rect shrinkCenterCoverRect(const Rect& rect) {
  const int insetWidth = rect.width - kCenterCoverVisualInset * 2;
  const int insetHeight = rect.height - kCenterCoverVisualInset * 2;
  const int width = std::max(0, insetWidth);
  const int height = std::max(0, insetHeight);
  return Rect{rect.x + (rect.width - width) / 2, rect.y + (rect.height - height) / 2, width, height};
}

Rect computeCenterCoverSlotRect(const GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks) {
  if (recentBooks.empty()) {
    const int screenW = renderer.getScreenWidth();
    const int fallbackX = (screenW - kDisplayCenterW) / 2;
    const int fallbackY = rect.y + kCoverTopPad + kCenterCoverTopInset - kCarouselVerticalLift;
    return Rect{fallbackX, fallbackY, kDisplayCenterW, kDisplayCenterH};
  }

  const int screenW = renderer.getScreenWidth();
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int reservedTitleBlockHeight = titleLineHeight * 2;
  const int titleY = rect.y + kTitleTopClearance;
  const int centerTileY = std::max(rect.y + kCoverTopPad, titleY + reservedTitleBlockHeight + kTitleBottomGap);
  const int centerDrawY = centerTileY + kCenterCoverTopInset - kCarouselVerticalLift;
  const int centerX = (screenW - kDisplayCenterW) / 2;
  return Rect{centerX, centerDrawY, kDisplayCenterW, kDisplayCenterH};
}

void drawMenuBookmarkIcon(const GfxRenderer& renderer, int x, int y, bool selected) {
  constexpr int ribbonWidth = 16;
  constexpr int ribbonHeight = 22;
  constexpr int notchSize = 6;
  const int iconX = x + (kMenuIconSize - ribbonWidth) / 2;
  const int iconY = y + 4;
  const int centerX = iconX + ribbonWidth / 2;

  const int polyX[5] = {iconX, iconX + ribbonWidth, iconX + ribbonWidth, centerX, iconX};
  const int polyY[5] = {iconY, iconY, iconY + ribbonHeight, iconY + ribbonHeight - notchSize, iconY + ribbonHeight};
  renderer.fillPolygon(polyX, polyY, 5, !selected);
}

void drawPerspectiveOutline(const GfxRenderer& renderer, int x, int y, int width, int leftHeight, int rightHeight) {
  const int maxHeight = std::max(leftHeight, rightHeight);
  const int topLeft = (maxHeight - leftHeight) / 2;
  const int topRight = (maxHeight - rightHeight) / 2;
  const int bottomLeft = topLeft + leftHeight - 1;
  const int bottomRight = topRight + rightHeight - 1;
  const int rightX = x + width - 1;

  renderer.drawLine(x, y + topLeft, rightX, y + topRight, kSideOutlineW, true);
  renderer.drawLine(x, y + bottomLeft, rightX, y + bottomRight, kSideOutlineW, true);
  renderer.fillRect(x, y + topLeft, kSideOutlineW, leftHeight, true);
  renderer.fillRect(rightX - kSideOutlineW + 1, y + topRight, kSideOutlineW, rightHeight, true);
  renderer.fillRect(x, y + maxHeight + 1, width, 2, false);
}

void fillPerspectiveSilhouette(const GfxRenderer& renderer, int x, int y, int width, int leftHeight, int rightHeight) {
  const int maxHeight = std::max(leftHeight, rightHeight);
  renderer.fillRect(x, y, width, maxHeight, false);
  for (int dx = 0; dx < width; ++dx) {
    const int columnHeight = (width <= 1) ? leftHeight : (leftHeight + ((rightHeight - leftHeight) * dx) / (width - 1));
    const int top = y + (maxHeight - columnHeight) / 2;
    renderer.fillRect(x + dx, top, 1, columnHeight, true);
  }
}

void formatCompactReadingTime(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }

  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
void LyraCarouselTheme::setPreRenderIndex(int idx) {
  lastCarouselSelectorIndex.store(idx, std::memory_order_relaxed);
  if (idx >= 0 && idx < LyraCarouselMetrics::values.homeRecentBooksCount) {
    const Rect cachedRect = cachedCenterCoverRects[idx];
    if (cachedRect.width > 0 && cachedRect.height > 0) lastCenterCoverRect = cachedRect;
  }
}

void LyraCarouselTheme::drawCarouselBorder(GfxRenderer& renderer, Rect coverRect,
                                           const std::vector<RecentBook>& recentBooks, int centerIdx,
                                           bool inCarouselRow) const {
  if (!inCarouselRow) return;
  Rect borderRect = shrinkCenterCoverRect(computeCenterCoverSlotRect(renderer, coverRect, recentBooks));
  renderer.drawRoundedRect(borderRect.x, borderRect.y, borderRect.width, borderRect.height, kSelectionLineW,
                           kCornerRadius, true);
}

// ---------------------------------------------------------------------------
// Carousel cover strip
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                            bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                            const std::function<bool()>& storeCoverBuffer,
                                            const BookReadingStats* stats, float progressPercent,
                                            const GlobalReadingStats* globalStats,
                                            const char* currentChapterTitle) const {
  (void)globalStats;
  (void)currentChapterTitle;
  // Reserved for future use: tells the carousel whether Home restored a cached frame buffer.
  (void)bufferRestored;
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  // When navigating the icon row, keep showing the last carousel position —
  // falling back to 0 on first use (lastCarouselSelectorIndex == -1).
  const bool inCarouselRow = (selectorIndex < bookCount);
  const int lastSelectorIndex = lastCarouselSelectorIndex.load(std::memory_order_relaxed);
  int centerIdx = inCarouselRow ? selectorIndex : (lastSelectorIndex >= 0 ? lastSelectorIndex : 0);

  if (centerIdx >= bookCount) {
    centerIdx = bookCount - 1;
    coverRendered = false;
    coverBufferStored = false;
  }

  // cppcheck-suppress knownConditionTrueFalse
  // Reachable as false when navigating the icon row with a previously-set
  // lastCarouselSelectorIndex; cppcheck only models the inCarouselRow=true path.
  if (centerIdx != lastSelectorIndex) {
    coverRendered = false;
    coverBufferStored = false;
  }

  const int screenW = renderer.getScreenWidth();
  const int textMaxWidth = std::min(screenW - 40, kCenterCoverMaxW + 40);
  const auto titleLines =
      renderer.wrappedText(kTitleFontId, recentBooks[centerIdx].title.c_str(), textMaxWidth, 2, EpdFontFamily::BOLD);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int reservedTitleBlockHeight = titleLineHeight * 2;
  const int titleY = rect.y + kTitleTopClearance;
  const int centerTileY = std::max(rect.y + kCoverTopPad, titleY + reservedTitleBlockHeight + kTitleBottomGap);
  const int sideMaxHeight = std::max(kNearSideInnerH, kNearSideOuterH);
  const Rect centerCoverSlotRect = computeCenterCoverSlotRect(renderer, rect, recentBooks);
  const int centerDrawY = centerCoverSlotRect.y;
  const int sideTileY = centerDrawY + (kDisplayCenterH - sideMaxHeight) / 2;

  const int centerX = centerCoverSlotRect.x;
  const int nearOverlap = 4;
  const int farOverlap = 2;
  constexpr int nearCoverInset = 10;
  const int baseLeftNearX = centerX - kNearSideW + nearOverlap;
  const int baseRightNearX = centerX + kDisplayCenterW - nearOverlap;
  const int leftNearX = baseLeftNearX + nearCoverInset;
  const int rightNearX = baseRightNearX - nearCoverInset;
  const int leftFarX = std::max(0, baseLeftNearX - kFarSideW + farOverlap);
  const int rightFarX = std::min(screenW - kFarSideW, baseRightNearX + kNearSideW - farOverlap);

  auto drawCenterCover = [&](int bookIdx, Rect& outRect) -> bool {
    if (bookIdx < 0 || bookIdx >= bookCount) return false;
    const RecentBook& book = recentBooks[bookIdx];
    outRect = shrinkCenterCoverRect(centerCoverSlotRect);

    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, kCenterThumbW, kCenterThumbH);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          const float srcW = static_cast<float>(bitmap.getWidth());
          const float srcH = static_cast<float>(bitmap.getHeight());
          const float srcRatio = srcW / srcH;
          const float safeTargetHeight = outRect.height == 0 ? 1.0f : static_cast<float>(outRect.height);
          const float targetRatio = static_cast<float>(outRect.width) / safeTargetHeight;
          float cropX = 0.0f;
          float cropY = 0.0f;

          if (srcRatio > targetRatio) {
            cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
          } else if (srcRatio < targetRatio) {
            cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
          }

          renderer.fillRect(outRect.x - kCenterOutlineW, outRect.y - kCenterOutlineW,
                            outRect.width + 2 * kCenterOutlineW, outRect.height + 2 * kCenterOutlineW, false);
          renderer.drawBitmap(bitmap, outRect.x, outRect.y, outRect.width, outRect.height, cropX, cropY);
          renderer.maskRoundedRectOutsideCorners(outRect.x, outRect.y, outRect.width, outRect.height, kCornerRadius,
                                                 Color::White);
          file.close();
          return true;
        }
        file.close();
      }
    }

    renderer.fillRect(outRect.x - kCenterOutlineW, outRect.y - kCenterOutlineW, outRect.width + 2 * kCenterOutlineW,
                      outRect.height + 2 * kCenterOutlineW, false);
    renderer.drawRoundedRect(outRect.x, outRect.y, outRect.width, outRect.height, 1, kCornerRadius, true);
    renderer.fillRoundedRect(outRect.x, outRect.y + outRect.height / 3, outRect.width, 2 * outRect.height / 3,
                             kCornerRadius, /*roundTopLeft=*/false, /*roundTopRight=*/false,
                             /*roundBottomLeft=*/true, /*roundBottomRight=*/true, Color::Black);
    constexpr int kFallbackTitlePadX = 14;
    constexpr int kFallbackTitlePadBottom = 14;
    constexpr int kFallbackIconGap = 10;
    const int iconX = outRect.x + outRect.width / 2 - 16;
    const int iconY = outRect.y + outRect.height / 3 + 14;
    renderer.drawIcon(CoverIcon, iconX, iconY, 32, 32);

    const int fallbackTitleX = outRect.x + kFallbackTitlePadX;
    const int fallbackTitleY = iconY + 32 + kFallbackIconGap;
    const int fallbackTitleW = outRect.width - kFallbackTitlePadX * 2;
    const int fallbackTitleH = outRect.y + outRect.height - kFallbackTitlePadBottom - fallbackTitleY;
    const int fallbackLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxFallbackLines = std::clamp(fallbackTitleH / std::max(1, fallbackLineHeight), 1, 4);
    const auto fallbackTitleLines =
        renderer.wrappedText(UI_10_FONT_ID, book.title.c_str(), fallbackTitleW, maxFallbackLines, EpdFontFamily::BOLD);
    const int fallbackBlockH = fallbackLineHeight * static_cast<int>(fallbackTitleLines.size());
    int fallbackLineY = fallbackTitleY + std::max(0, (fallbackTitleH - fallbackBlockH) / 2);
    for (const auto& line : fallbackTitleLines) {
      const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, outRect.x + (outRect.width - lineW) / 2, fallbackLineY, line.c_str(), false,
                        EpdFontFamily::BOLD);
      fallbackLineY += fallbackLineHeight;
    }
    return false;
  };

  auto drawSideCover = [&](int bookIdx, int x, int width, int leftHeight, int rightHeight) -> bool {
    if (bookIdx < 0 || bookIdx >= bookCount) return false;
    const RecentBook& book = recentBooks[bookIdx];

    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, kSideCoverMaxW, kSideCoverMaxH);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const int sideHeight = std::max(leftHeight, rightHeight);
          renderer.fillRect(x, sideTileY, width, sideHeight, false);
          renderer.drawPerspectiveBitmap(bitmap, x, sideTileY, width, leftHeight, rightHeight);
          renderer.maskRoundedRectOutsideCorners(x, sideTileY, width, sideHeight, kSideCornerRadius, Color::White);
          file.close();
          drawPerspectiveOutline(renderer, x, sideTileY, width, leftHeight, rightHeight);
          return true;
        }
        file.close();
      }
    }

    fillPerspectiveSilhouette(renderer, x, sideTileY, width, leftHeight, rightHeight);
    renderer.maskRoundedRectOutsideCorners(x, sideTileY, width, std::max(leftHeight, rightHeight), kSideCornerRadius,
                                           Color::White);
    return false;
  };

  if (!coverRendered) {
    lastCarouselSelectorIndex.store(centerIdx, std::memory_order_relaxed);

    // Clear the entire cover tile to white so stale pixels from old positions
    // don't persist (drawBitmap only sets black pixels, never clears).
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    // More literal Lyra Flow layout: two visible books per side when available.
    const int leftNearIdx = (centerIdx + bookCount - 1) % bookCount;
    const int leftFarIdx = (centerIdx + bookCount - 2) % bookCount;
    const int rightNearIdx = (centerIdx + 1) % bookCount;
    const int rightFarIdx = (centerIdx + 2) % bookCount;

    if (bookCount >= 5) drawSideCover(leftFarIdx, leftFarX, kFarSideW, kFarSideInnerH, kFarSideOuterH);
    if (bookCount >= 4) drawSideCover(rightFarIdx, rightFarX, kFarSideW, kFarSideOuterH, kFarSideInnerH);
    if (bookCount >= 2) drawSideCover(leftNearIdx, leftNearX, kNearSideW, kNearSideInnerH, kNearSideOuterH);
    if (bookCount >= 3) drawSideCover(rightNearIdx, rightNearX, kNearSideW, kNearSideOuterH, kNearSideInnerH);

    Rect centerCoverRect{};
    drawCenterCover(centerIdx, centerCoverRect);
    lastCenterCoverRect = centerCoverRect;
    if (centerIdx >= 0 && centerIdx < LyraCarouselMetrics::values.homeRecentBooksCount) {
      cachedCenterCoverRects[centerIdx] = centerCoverRect;
    }

    // Title sits above the center cover; wrap to 2 lines and ellipsize on line 2 if needed.
    const int textCenterX = centerCoverRect.x + centerCoverRect.width / 2;
    const int titleVerticalInset = (reservedTitleBlockHeight - titleBlockHeight) / 2;
    int currentTitleY = titleY + titleVerticalInset + kTitleDrawOffset;
    for (const auto& titleLine : titleLines) {
      const int titleW = renderer.getTextWidth(kTitleFontId, titleLine.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(kTitleFontId, textCenterX - titleW / 2, currentTitleY, titleLine.c_str(), true,
                        EpdFontFamily::BOLD);
      currentTitleY += titleLineHeight;
    }

    // Dots — centred under the displayed centre cover, count = actual book count
    const int dotsY = centerCoverSlotRect.y + centerCoverSlotRect.height + 8;
    const int totalDotsW = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = centerCoverSlotRect.x + (centerCoverSlotRect.width - totalDotsW) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIdx)
        renderer.fillRect(dotX, dotsY, kDotSize, kDotSize, true);
      else
        renderer.drawRect(dotX, dotsY, kDotSize, kDotSize, true);
      dotX += kDotSize + kDotGap;
    }

    // Minimal-style reading progress footer below the cover.
    constexpr int footerLabelFontId = UI_10_FONT_ID;
    const int footerLabelLineHeight = renderer.getLineHeight(footerLabelFontId);
    const bool hasStats = (stats != nullptr && stats->sessionCount > 0);
    const bool hasProgress = progressPercent >= 0.0f;
    int infoY = dotsY + kDotSize + kFooterTopGap;
    const int footerMaxWidth = std::max(0, screenW - 2 * LyraCarouselMetrics::values.contentSidePadding);
    const int footerWidth = std::min(footerMaxWidth, centerCoverRect.width);
    const int footerX = centerCoverRect.x + (centerCoverRect.width - footerWidth) / 2;

    if (hasStats) {
      char buf[48];
      formatCompactReadingTime(stats->totalReadingSeconds, buf, sizeof(buf));
      const auto timeLabel = renderer.truncatedText(footerLabelFontId, buf, footerWidth, EpdFontFamily::REGULAR);
      renderer.drawText(footerLabelFontId, footerX, infoY, timeLabel.c_str(), true, EpdFontFamily::REGULAR);
    }

    if (hasProgress) {
      const int progressBarY = infoY + (hasStats ? footerLabelLineHeight + kFooterLabelToBarGap : 0);
      const float clampedProgress = std::clamp(progressPercent, 0.0f, 100.0f);
      const int filledWidth = std::clamp(static_cast<int>((clampedProgress / 100.0f) * footerWidth), 0, footerWidth);
      char progressLabel[16];
      snprintf(progressLabel, sizeof(progressLabel), "%.0f%%", clampedProgress);
      renderer.fillRectDither(footerX, progressBarY, footerWidth, kFooterProgressBarHeight, Color::LightGray);
      if (filledWidth > 0) {
        renderer.fillRect(footerX, progressBarY, filledWidth, kFooterProgressBarHeight, true);
      }
      const int progressLabelW = renderer.getTextWidth(footerLabelFontId, progressLabel, EpdFontFamily::REGULAR);
      const int progressLabelY = progressBarY + kFooterProgressBarHeight + kFooterPercentTopGap;
      renderer.drawText(footerLabelFontId, footerX + footerWidth - progressLabelW, progressLabelY, progressLabel, true,
                        EpdFontFamily::REGULAR);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  } else if (lastCenterCoverRect.width <= 0 || lastCenterCoverRect.height <= 0) {
    lastCenterCoverRect = shrinkCenterCoverRect(centerCoverSlotRect);
  }

  // Always outline the centre cover at its own edge (white ring sits outside the black line);
  // thicker when the carousel row is active
  const int outlineW = inCarouselRow ? kSelectionLineW : kThinOutlineW;
  renderer.drawRoundedRect(lastCenterCoverRect.x, lastCenterCoverRect.y, lastCenterCoverRect.width,
                           lastCenterCoverRect.height, outlineW, kCornerRadius, true);
}

// ---------------------------------------------------------------------------
// Horizontal icon-only menu row — anchored to bottom of screen
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                       const std::function<std::string(int index)>& buttonLabel,
                                       const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;
  // Rect is retained by the BaseTheme interface; this carousel menu anchors to the screen bottom.

  const MenuLayoutMetrics metrics = computeMenuLayout(renderer, buttonCount);

  for (int i = 0; i < buttonCount; ++i) {
    const int tileX = i * metrics.tileW;
    const int iconX = tileX + (metrics.tileW - kMenuIconSize) / 2;
    const int iconY = metrics.rowY + kMenuIconPad;

    const bool selected = (selectedIndex == i);
    if (selected) {
      const int highlightSize = kMenuIconSize + 2 * kHighlightPad;
      const int highlightY = metrics.rowY + (metrics.tileH - highlightSize) / 2;
      renderer.fillRoundedRect(iconX - kHighlightPad, highlightY, highlightSize, highlightSize, kCornerRadius,
                               Color::Black);
    }

    if (rowIcon != nullptr) {
      const UIIcon icon = rowIcon(i);
      if (icon == UIIcon::BookmarkIcon) {
        drawMenuBookmarkIcon(renderer, iconX, iconY, selected);
      } else {
        const uint8_t* bmp = iconForName(icon, kMenuIconSize);
        if (bmp != nullptr) {
          if (selected)
            renderer.drawIconInverted(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
          else
            renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
        }
      }
    }
  }

  renderer.fillRect(0, metrics.labelY, renderer.getScreenWidth(), metrics.labelLineHeight, false);
  if (selectedIndex >= 0 && selectedIndex < buttonCount && buttonLabel != nullptr) {
    const std::string labelStr = buttonLabel(selectedIndex);
    const auto centeredLabel =
        renderer.truncatedText(kMenuLabelFontId, labelStr.c_str(), renderer.getScreenWidth() - 40);
    const int labelWidth = renderer.getTextWidth(kMenuLabelFontId, centeredLabel.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kMenuLabelFontId, (renderer.getScreenWidth() - labelWidth) / 2, metrics.labelY + 2,
                      centeredLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
}

void LyraCarouselTheme::drawButtonMenuSelectionOverlay(const GfxRenderer& renderer, int buttonCount, int selectedIndex,
                                                       const std::function<std::string(int index)>& buttonLabel,
                                                       const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0 || selectedIndex < 0 || selectedIndex >= buttonCount) return;

  const MenuLayoutMetrics metrics = computeMenuLayout(renderer, buttonCount);

  const int tileX = selectedIndex * metrics.tileW;
  const int iconX = tileX + (metrics.tileW - kMenuIconSize) / 2;
  const int iconY = metrics.rowY + kMenuIconPad;
  const int highlightSize = kMenuIconSize + 2 * kHighlightPad;
  const int highlightY = metrics.rowY + (metrics.tileH - highlightSize) / 2;

  renderer.fillRoundedRect(iconX - kHighlightPad, highlightY, highlightSize, highlightSize, kCornerRadius,
                           Color::Black);

  if (rowIcon != nullptr) {
    const UIIcon icon = rowIcon(selectedIndex);
    if (icon == UIIcon::BookmarkIcon) {
      drawMenuBookmarkIcon(renderer, iconX, iconY, true);
    } else {
      const uint8_t* bmp = iconForName(icon, kMenuIconSize);
      if (bmp != nullptr) {
        renderer.drawIconInverted(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
      }
    }
  }

  renderer.fillRect(0, metrics.labelY, renderer.getScreenWidth(), metrics.labelLineHeight, false);
  if (buttonLabel != nullptr) {
    const std::string labelStr = buttonLabel(selectedIndex);
    const auto centeredLabel =
        renderer.truncatedText(kMenuLabelFontId, labelStr.c_str(), renderer.getScreenWidth() - 40);
    const int labelWidth = renderer.getTextWidth(kMenuLabelFontId, centeredLabel.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kMenuLabelFontId, (renderer.getScreenWidth() - labelWidth) / 2, metrics.labelY + 2,
                      centeredLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
}

// ---------------------------------------------------------------------------
// List — solid black highlight, inverted text and icons on selected row
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                 const std::function<std::string(int index)>& rowTitle,
                                 const std::function<std::string(int index)>& rowSubtitle,
                                 const std::function<UIIcon(int index)>& rowIcon,
                                 const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                 const std::function<bool(int index)>& rowDimmed,
                                 const std::function<bool(int index)>& isHeader) const {
  drawListWithMetrics(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                      highlightValue, rowDimmed, isHeader, LyraCarouselMetrics::values, true);
}
