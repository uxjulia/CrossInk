#include "MinimalTheme.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/UITheme.h"
#include "components/icons/afternoon.h"
#include "components/icons/cover.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "fontIds.h"

namespace {
struct MinimalQuote {
  const char* text;
  const char* author;
};

constexpr MinimalQuote kQuotes[] = {
    {"\"A book is a dream you hold in your hands.\"", "Neil Gaiman"},
    {"\"I have always imagined that Paradise will be a kind of library.\"", "Jorge Luis Borges"},
    {"\"A reader lives a thousand lives before he dies. The man who never reads lives only one.\"",
     "George R.R. Martin"},
    {"\"So many books, so little time.\"", "Frank Zappa"},
    {"\"If you only read the books that everyone else is reading, you can only think what everyone else is thinking.\"",
     "Haruki Murakami"},
};

constexpr int kCoverCornerRadius = 8;
constexpr int kProgressBarHeight = 6;
constexpr int kButtonCornerRadius = 6;
constexpr int kFileBrowserIconSize = 24;
constexpr int kFileBrowserRowVerticalPadding = 6;
constexpr int kFileBrowserTextGap = 8;
constexpr int kFileBrowserValueMaxWidth = 76;
constexpr int kMenuPanelWidth = 384;
constexpr int kMenuRowHeight = 64;
constexpr int kMenuPanelTop = 210;
constexpr int kMenuPanelRadius = 3;
constexpr int kMenuSelectionTriangleWidth = 14;
constexpr int kMenuSelectionTriangleHeight = 20;
constexpr int kMenuSelectionTriangleInset = 30;
constexpr int kCoverTopOffset = 0;
constexpr int kProgressBlockGap = 8;
constexpr int kProgressBarGap = 4;
constexpr int kProgressLabelGap = 5;
constexpr int kStatsFooterReaderIconSize = 24;
constexpr int kStatsFooterStreakIconSize = 24;
constexpr int kStatsFooterBottomInset = 34;
constexpr int kStatsFooterSideInset = 48;
constexpr int kStatsFooterColumnGap = 18;
int homeButtonHintSelection = -1;

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats& globalStats) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

void drawReaderTypeIcon(const GfxRenderer& renderer, const GlobalReadingStats& globalStats, const int x, const int y) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    renderer.drawIconInverted(NightReaderIcon, x, y, kStatsFooterReaderIconSize, kStatsFooterReaderIconSize);
    return;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      renderer.drawIconInverted(MorningReaderIcon, x, y, kStatsFooterReaderIconSize, kStatsFooterReaderIconSize);
      return;
    case ReadingTimeBucket::Afternoon:
      renderer.drawIconInverted(AfternoonReaderIcon, x, y, kStatsFooterReaderIconSize, kStatsFooterReaderIconSize);
      return;
    case ReadingTimeBucket::Evening:
      renderer.drawIconInverted(EveningReaderIcon, x, y, kStatsFooterReaderIconSize, kStatsFooterReaderIconSize);
      return;
    case ReadingTimeBucket::Night:
    default:
      renderer.drawIconInverted(NightReaderIcon, x, y, kStatsFooterReaderIconSize, kStatsFooterReaderIconSize);
      return;
  }
}

void formatStreakStat(const GlobalReadingStats& globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats.currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  snprintf(buf, len, tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak), dayCountText(streak));
}

void drawStatsFooter(const GfxRenderer& renderer, const GlobalReadingStats& globalStats) {
  if (!gpio.deviceIsX3()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int labelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowHeight = std::max({labelLineHeight, kStatsFooterStreakIconSize, kStatsFooterReaderIconSize});
  const int topY = screenHeight - kStatsFooterBottomInset - rowHeight;
  const int streakIconY = topY + (rowHeight - kStatsFooterStreakIconSize) / 2;
  const int readerIconY = topY + (rowHeight - kStatsFooterReaderIconSize) / 2;
  const int textY = topY + (rowHeight - labelLineHeight) / 2;
  const int availableWidth = screenWidth - kStatsFooterSideInset * 2;
  const int iconTextGap = 10;

  char streakBuf[48];
  formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));
  const char* readerLabel = readerTypeLabel(globalStats);

  const std::string streakText =
      renderer.truncatedText(UI_10_FONT_ID, streakBuf, availableWidth / 2 - (kStatsFooterStreakIconSize + iconTextGap));
  const std::string readerText = renderer.truncatedText(
      UI_10_FONT_ID, readerLabel, availableWidth / 2 - (kStatsFooterReaderIconSize + iconTextGap));

  const int streakBlockWidth =
      kStatsFooterStreakIconSize + iconTextGap + renderer.getTextWidth(UI_10_FONT_ID, streakText.c_str());
  const int readerBlockWidth =
      kStatsFooterReaderIconSize + iconTextGap + renderer.getTextWidth(UI_10_FONT_ID, readerText.c_str());
  const int contentWidth = availableWidth;
  const int contentLeft = (screenWidth - contentWidth) / 2;
  const int leftX = contentLeft;
  const int rightX =
      std::max(contentLeft + contentWidth - readerBlockWidth, leftX + streakBlockWidth + kStatsFooterColumnGap);
  const int streakTextX = leftX + kStatsFooterStreakIconSize + iconTextGap;
  const int readerTextX = rightX + kStatsFooterReaderIconSize + iconTextGap;

  renderer.drawIconInverted(StreakIcon, leftX, streakIconY, kStatsFooterStreakIconSize, kStatsFooterStreakIconSize);
  renderer.drawText(UI_10_FONT_ID, streakTextX, textY, streakText.c_str(), false);

  drawReaderTypeIcon(renderer, globalStats, rightX, readerIconY);
  renderer.drawText(UI_10_FONT_ID, readerTextX, textY, readerText.c_str(), false);
}

Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect) {
  const int coverH = MinimalMetrics::values.homeCoverHeight;
  const int coverW = MinimalMetrics::homeCoverWidth;
  const int coverX = (renderer.getScreenWidth() - coverW) / 2;
  const int coverY = rect.y + kCoverTopOffset;
  return Rect{coverX, coverY, coverW, coverH};
}

Rect coverImageRectForFrame(const Rect& coverRect) {
  const int imageW = std::min(coverRect.width, MinimalMetrics::homeCoverImageWidth);
  const int imageH = std::min(coverRect.height, MinimalMetrics::homeCoverImageHeight);
  return Rect{coverRect.x + (coverRect.width - imageW) / 2, coverRect.y + (coverRect.height - imageH) / 2, imageW,
              imageH};
}

Rect fittedBitmapRect(const Bitmap& bitmap, const Rect& target) {
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || target.width <= 0 || target.height <= 0) {
    return target;
  }

  const float widthScale = static_cast<float>(target.width) / static_cast<float>(bitmap.getWidth());
  const float heightScale = static_cast<float>(target.height) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(1.0f, std::min(widthScale, heightScale));
  const int drawnW = std::min(target.width, std::max(1, static_cast<int>(std::ceil(bitmap.getWidth() * scale))));
  const int drawnH = std::min(target.height, std::max(1, static_cast<int>(std::ceil(bitmap.getHeight() * scale))));
  return Rect{target.x + (target.width - drawnW) / 2, target.y + (target.height - drawnH) / 2, drawnW, drawnH};
}

std::string coverPathForImageRect(const RecentBook& book, const Rect& imageRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }

  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(imageRect.width, imageRect.height);
  }

  std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
  if (coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) {
    coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.height);
  }
  return coverBmpPath;
}

uint8_t selectedQuoteIndex() {
  static bool initialized = false;
  static uint8_t index = 0;
  if (!initialized) {
    index = static_cast<uint8_t>((millis() / 137u) % (sizeof(kQuotes) / sizeof(kQuotes[0])));
    initialized = true;
  }
  return index;
}

int centeredRowY(const int rowY, const int rowHeight, const int contentHeight) {
  return rowY + std::max(0, rowHeight - contentHeight) / 2;
}

void drawProgressBlock(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                       float progressPercent, const bool inverted) {
  if ((stats == nullptr || stats->totalReadingSeconds == 0) && progressPercent < 0.0f) {
    return;
  }

  const int barW = coverRect.width;
  const int barX = coverRect.x;
  const int durationY = coverRect.y + coverRect.height + kProgressBlockGap;
  const int barY = durationY + renderer.getLineHeight(UI_10_FONT_ID) + kProgressBarGap;
  const bool textBlack = !inverted;

  if (stats != nullptr && stats->totalReadingSeconds > 0) {
    char duration[32];
    BookReadingStats::formatDuration(stats->totalReadingSeconds, duration, sizeof(duration));
    renderer.drawText(UI_10_FONT_ID, barX, durationY, duration, textBlack);
  }

  if (progressPercent < 0.0f) {
    return;
  }

  const int progress = std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100);
  const int fillW = (barW * progress) / 100;
  if (inverted) {
    renderer.drawRect(barX, barY, barW, kProgressBarHeight, false);
    if (fillW > 0) {
      renderer.fillRect(barX, barY, fillW, kProgressBarHeight, false);
    }
  } else {
    renderer.fillRectDither(barX, barY, barW, kProgressBarHeight, Color::LightGray);
    if (fillW > 0) {
      renderer.fillRectDither(barX, barY, fillW, kProgressBarHeight, Color::DarkGray);
    }
  }

  char progressLabel[12];
  snprintf(progressLabel, sizeof(progressLabel), "%d%%", progress);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, progressLabel);
  renderer.drawText(UI_10_FONT_ID, barX + barW - labelW, barY + kProgressBarHeight + kProgressLabelGap, progressLabel,
                    textBlack);
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  constexpr int commonBookCoverHeightRatio = 3;
  constexpr int commonBookCoverWidthRatio = 2;
  const int placeholderHeight =
      std::min(coverRect.height, (coverRect.width * commonBookCoverHeightRatio) / commonBookCoverWidthRatio);
  const Rect placeholderRect{coverRect.x, coverRect.y + (coverRect.height - placeholderHeight) / 2, coverRect.width,
                             placeholderHeight};

  renderer.fillRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height,
                           kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height, 1,
                           kCoverCornerRadius, true);

  const int dividerY = placeholderRect.y + placeholderRect.height / 3;
  renderer.drawLine(placeholderRect.x, dividerY, placeholderRect.x + placeholderRect.width - 1, dividerY, true);

  constexpr int iconSize = 32;
  renderer.drawIcon(CoverIcon, placeholderRect.x + (placeholderRect.width - iconSize) / 2,
                    placeholderRect.y + (placeholderRect.height / 3 - iconSize) / 2, iconSize, iconSize);

  constexpr int textPadding = 16;
  constexpr int textVerticalPadding = 22;
  constexpr int titleAuthorGap = 28;
  const int textW = placeholderRect.width - textPadding * 2;
  const std::string& titleText = book.title.empty() ? book.path : book.title;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasAuthor = !book.author.empty();
  auto authorLines =
      hasAuthor ? renderer.wrappedText(UI_10_FONT_ID, book.author.c_str(), textW, 2) : std::vector<std::string>{};
  const int lowerAreaHeight = placeholderRect.y + placeholderRect.height - dividerY;
  const int authorBlockHeight = authorLineHeight * static_cast<int>(authorLines.size());
  const int authorGap = authorLines.empty() ? 0 : titleAuthorGap;
  const int availableTitleHeight = lowerAreaHeight - textVerticalPadding * 2 - authorBlockHeight - authorGap;
  const int maxTitleLines = std::clamp(availableTitleHeight / titleLineHeight, 1, 4);
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, titleText.c_str(), textW, maxTitleLines);

  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int totalTextHeight = titleBlockHeight + authorBlockHeight + authorGap;
  int textY = dividerY + std::max(textVerticalPadding, (lowerAreaHeight - totalTextHeight) / 2);

  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
    textY += titleLineHeight;
  }

  if (!authorLines.empty()) {
    textY += titleAuthorGap;
    for (const auto& line : authorLines) {
      const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
      textY += authorLineHeight;
    }
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  if (!book.coverBmpPath.empty()) {
    const Rect imageRect = coverImageRectForFrame(coverRect);
    const std::string coverBmpPath = coverPathForImageRect(book, imageRect);
    if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const Rect bitmapRect = fittedBitmapRect(bitmap, imageRect);
          renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                   backgroundColor);
          renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, kCoverCornerRadius,
                                   Color::White);
          renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
          renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                                 kCoverCornerRadius, backgroundColor);
          renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1,
                                   kCoverCornerRadius, true);
          hasCover = true;
        }
        file.close();
      }
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}
}  // namespace

void MinimalTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                              const bool readerContext) const {
  (void)subtitle;

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - MinimalMetrics::values.batteryWidth;
  const int batteryY = rect.y + homeHeaderTopInset;
  drawBatteryRight(renderer,
                   Rect{batteryX, batteryY, MinimalMetrics::values.batteryWidth, MinimalMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    constexpr int titleInsetX = 12;
    const int maxTitleWidth = batteryX - rect.x - titleInsetX - MinimalMetrics::values.contentSidePadding;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + titleInsetX, rect.y + MinimalMetrics::values.batteryBarHeight + 3,
                      truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  drawTopStatusBarClock(renderer, rect.y, nullptr, readerContext,
                        title == nullptr && !readerContext ? homeHeaderClockTextYOffset(renderer) : 0);
}

void MinimalTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                              const bool selected) const {
  (void)selected;

  if (tabs.empty()) {
    return;
  }

  const int tabCount = static_cast<int>(tabs.size());
  const int lineY = rect.y + rect.height - 1;
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  renderer.drawLine(rect.x, lineY, rect.x + rect.width - 1, lineY, true);

  for (int i = 0; i < tabCount; i++) {
    const int slotX = rect.x + (i * rect.width) / tabCount;
    const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
    const int slotWidth = nextSlotX - slotX;
    const auto& tab = tabs[i];
    const auto fontStyle = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, fontStyle);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, tab.label, true, fontStyle);
  }
}

int MinimalTheme::compactFileBrowserRowHeight(const GfxRenderer& renderer) const {
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kFileBrowserRowVerticalPadding;
  return std::max(kFileBrowserIconSize + kFileBrowserRowVerticalPadding, textHeight);
}

void MinimalTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed,
                            const std::function<bool(int index)>& isHeader) const {
  const bool compactFileRows = rowSubtitle != nullptr && rowIcon != nullptr && rowValue != nullptr;
  if (!compactFileRows) {
    LyraTheme::drawList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                        highlightValue, rowDimmed, isHeader);
    return;
  }

  if (itemCount <= 0) return;

  const int fileRowHeight = compactFileBrowserRowHeight(renderer);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int folderRowHeight = MinimalMetrics::values.listRowHeight;
  const auto isFolderRow = [&](int index) { return rowSubtitle(index) == "folder"; };
  const auto rowHeightFor = [&](int index) { return isFolderRow(index) ? folderRowHeight : fileRowHeight; };
  const auto pageEndFor = [&](int startIndex) {
    int usedHeight = 0;
    int endIndex = startIndex;
    while (endIndex < itemCount) {
      const int nextRowHeight = rowHeightFor(endIndex);
      if (endIndex > startIndex && usedHeight + nextRowHeight > rect.height) break;
      usedHeight += nextRowHeight;
      endIndex++;
    }
    return std::max(startIndex + 1, endIndex);
  };

  int pageStartIndex = 0;
  int pageEndIndex = pageEndFor(pageStartIndex);
  while (selectedIndex >= pageEndIndex && pageEndIndex < itemCount) {
    pageStartIndex = pageEndIndex;
    pageEndIndex = pageEndFor(pageStartIndex);
  }

  int totalPages = 0;
  int currentPage = 0;
  for (int pageStart = 0; pageStart < itemCount;) {
    if (pageStart == pageStartIndex) currentPage = totalPages;
    totalPages++;
    const int nextPageStart = pageEndFor(pageStart);
    if (nextPageStart <= pageStart) break;
    pageStart = nextPageStart;
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? (MinimalMetrics::values.scrollBarWidth + MinimalMetrics::values.scrollBarRightOffset) : 1);

  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(MinimalMetrics::values.scrollBarWidth, scrollAreaHeight / totalPages);
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - MinimalMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - MinimalMetrics::values.scrollBarWidth, scrollBarY,
                      MinimalMetrics::values.scrollBarWidth, scrollBarHeight, true);
  }

  if (selectedIndex >= 0) {
    int selectedY = rect.y;
    for (int i = pageStartIndex; i < selectedIndex; i++) {
      selectedY += rowHeightFor(i);
    }
    const int selectedRowHeight = rowHeightFor(selectedIndex);
    renderer.fillRoundedRect(rect.x + MinimalMetrics::values.contentSidePadding, selectedY,
                             contentWidth - MinimalMetrics::values.contentSidePadding * 2, selectedRowHeight, 6,
                             Color::LightGray);
  }

  const int iconX = rect.x + MinimalMetrics::values.contentSidePadding + kFileBrowserTextGap;
  const int textX = iconX + kFileBrowserIconSize + kFileBrowserTextGap;

  int itemY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageEndIndex; i++) {
    const int rowHeight = rowHeightFor(i);
    const bool folderRow = isFolderRow(i);
    const bool selectedRow = i == selectedIndex;

    std::string valueText = rowValue(i);
    if (!valueText.empty()) {
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), kFileBrowserValueMaxWidth);
    }
    const int valueWidth = valueText.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    const int valueGap = valueText.empty() ? 0 : kFileBrowserTextGap;
    const int textWidth =
        std::max(1, contentWidth - textX - MinimalMetrics::values.contentSidePadding - valueWidth - valueGap);

    const uint8_t* iconBitmap = iconForName(rowIcon(i), kFileBrowserIconSize);
    if (iconBitmap != nullptr) {
      const int iconY = centeredRowY(itemY, rowHeight, kFileBrowserIconSize);
      renderer.drawIcon(iconBitmap, iconX, iconY, kFileBrowserIconSize, kFileBrowserIconSize);
    }

    const int maxTitleLines = folderRow ? 1 : 2;
    auto lines = renderer.wrappedText(UI_10_FONT_ID, rowTitle(i).c_str(), textWidth, maxTitleLines);
    const int textBlockHeight = static_cast<int>(lines.size()) * lineHeight;
    int textY = centeredRowY(itemY, rowHeight, textBlockHeight);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true);
      textY += lineHeight;
    }

    if (!valueText.empty()) {
      const int valueY = centeredRowY(itemY, rowHeight, lineHeight);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - MinimalMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }

    if (rowDimmed && rowDimmed(i) && !selectedRow) {
      const int dimHeight = std::max(lineHeight, textBlockHeight);
      for (int py = itemY; py < itemY + dimHeight; py++) {
        for (int px = textX; px < textX + textWidth; px++) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }
    itemY += rowHeight;
  }
}

void MinimalTheme::setHomeButtonHintSelection(const int selectedIndex) { homeButtonHintSelection = selectedIndex; }

void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4, const bool allowInvertedText) const {
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && origOrientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = MinimalMetrics::values.buttonHintsHeight;
  constexpr int buttonY = MinimalMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = screenWidth > 500 ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int selectedIndex = homeButtonHintSelection;
  homeButtonHintSelection = -1;

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    const bool hasLabel = labels[i] != nullptr && labels[i][0] != '\0';
    if (hasLabel) {
      const Color background = i == selectedIndex ? Color::LightGray : Color::White;
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, kButtonCornerRadius, background);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    } else {
      const int smallButtonY = pageHeight - smallButtonHeight;
      renderer.fillRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, kButtonCornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }

  renderer.setOrientation(origOrientation);
}

void MinimalTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                       const BookReadingStats* stats, float progressPercent) const {
  (void)selectorIndex;
  (void)bufferRestored;

  const Rect coverRect = coverRectForScreen(renderer, rect);
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

    const MinimalQuote& quote = kQuotes[selectedQuoteIndex()];
    constexpr int quotePadding = 18;
    const int textW = coverRect.width - quotePadding * 2;
    auto lines = renderer.wrappedText(UI_12_FONT_ID, quote.text, textW, 6);
    int lineY = coverRect.y + 88;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, coverRect.x + quotePadding, lineY, line.c_str());
      lineY += lineH;
    }

    const int authorW = renderer.getTextWidth(UI_10_FONT_ID, quote.author, EpdFontFamily::ITALIC);
    renderer.drawText(UI_10_FONT_ID, coverRect.x + coverRect.width - quotePadding - authorW,
                      coverRect.y + coverRect.height - 110, quote.author, true, EpdFontFamily::ITALIC);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  drawProgressBlock(renderer, coverRect, stats, progressPercent, false);
}

void MinimalTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                   const float progressPercent) const {
  renderer.clearScreen(0x00);

  const Rect contentRect{0, MinimalMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                         MinimalMetrics::values.homeCoverTileHeight};
  const Rect coverRect = coverRectForScreen(renderer, contentRect);
  drawBookCover(renderer, coverRect, book, Color::Black);
  drawProgressBlock(renderer, coverRect, stats, progressPercent, true);
}

void MinimalTheme::drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book,
                                        const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                                        const float progressPercent) const {
  drawSleepScreen(renderer, book, stats, progressPercent);
  if (globalStats != nullptr) {
    drawStatsFooter(renderer, *globalStats);
  }
}

void MinimalTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rect;
  (void)rowIcon;

  if (buttonCount <= 0) {
    return;
  }

  const int panelW = std::min(kMenuPanelWidth, renderer.getScreenWidth() - 80);
  const int panelH = buttonCount * kMenuRowHeight + 2;
  const int panelX = (renderer.getScreenWidth() - panelW) / 2;
  const int panelY = kMenuPanelTop;
  renderer.drawRoundedRect(panelX, panelY, panelW, panelH, 1, kMenuPanelRadius, true);

  for (int i = 0; i < buttonCount; ++i) {
    const int rowY = panelY + 1 + i * kMenuRowHeight;
    if (i == selectedIndex) {
      const int triangleX = panelX + kMenuSelectionTriangleInset;
      const int triangleCenterY = rowY + kMenuRowHeight / 2;
      const int triangleHalfH = kMenuSelectionTriangleHeight / 2;
      const int triangleXPoints[3] = {triangleX, triangleX, triangleX + kMenuSelectionTriangleWidth};
      const int triangleYPoints[3] = {triangleCenterY - triangleHalfH, triangleCenterY + triangleHalfH,
                                      triangleCenterY};
      renderer.fillPolygon(triangleXPoints, triangleYPoints, 3, true);
    }

    const std::string label = buttonLabel(i);
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str());
    const int labelY = rowY + (kMenuRowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, panelX + (panelW - labelW) / 2, labelY, label.c_str());
  }
}
