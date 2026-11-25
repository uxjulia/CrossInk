#include "DashboardTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "components/themes/minimal/CalibratedThemeLayout.h"
#include "fontIds.h"

namespace {
constexpr int kContentInsetX4 = 20;
constexpr int kContentInsetX3 = 75;
constexpr int kTopInset = 20;
constexpr int kCoverCornerRadius = 8;
constexpr int kStatsColumnWidth = 105;
constexpr int kStatsColumnWidthWide = 120;
constexpr int kCoverStatsGap = 15;
constexpr int kPairInwardShiftX3 = 15;
#if FREEINK_DEVICE_EEGO_A4
constexpr int kExtraContentInset = 20;
#else
constexpr int kExtraContentInset = 0;
#endif
constexpr int kTitleTopGap = 28;
constexpr int kTitleChapterGap = 8;
constexpr int kBookTitleMaxLines = 2;
constexpr int kBookChapterMaxLines = 2;
constexpr int kFooterIconSize = 24;
constexpr int kFooterIconTextGap = 18;
constexpr int kFooterBottomGap = 57;
constexpr int kStatsRowCount = 7;
constexpr int kStatsRowCountX4 = 6;
constexpr int kStatsValueLabelGap = 1;

bool isWideScreen(const GfxRenderer& renderer) { return renderer.getScreenWidth() >= 560; }

struct DashboardLayout {
  Rect cover;
  Rect stats;
  Rect text;
  Rect footer;
};

DashboardLayout layoutForContent(const GfxRenderer& renderer, Rect content) {
  const auto edges = renderer.getViewableInsets().rotated(static_cast<unsigned>(renderer.getOrientation())).edges;
  const int inset = isWideScreen(renderer) ? kContentInsetX3 : kContentInsetX4;
  const int shift = (gpio.deviceIsX3() ? kPairInwardShiftX3 : 0) + kExtraContentInset;
  const int left = std::max(content.x, inset) + shift;
  const int right =
      std::min(content.x + content.width, renderer.getScreenWidth() - std::max<int>(inset, edges[1])) - shift;
  content.x = left;
  content.width = std::max(0, right - left);
  const int topInset = std::min(kTopInset, content.height);
  content.y += topInset;
  content.height -= topInset;

  const int footerH = halClock.isAvailable() ? std::max(kFooterIconSize, renderer.getLineHeight(UI_10_FONT_ID))
                                             : renderer.getLineHeight(UI_12_FONT_ID) + kStatsValueLabelGap +
                                                   renderer.getLineHeight(UI_10_FONT_ID);
  const int bottom = content.y + content.height;
  const int footerCenter = std::max(content.y + footerH / 2, bottom - std::max(kFooterBottomGap, (footerH + 1) / 2));
  const Rect footer{content.x, footerCenter - footerH / 2, content.width, footerH};
  // Tracking can change while Home retains its cover buffer. Keep the artwork
  // geometry stable; disabling stats only hides the footer, not its reserved space.
  const int textBottom = footer.y - kTitleChapterGap;
  const int textH =
      (kBookTitleMaxLines + kBookChapterMaxLines) * renderer.getLineHeight(renderer.filenameFontId(UI_12_FONT_ID)) +
      kTitleTopGap + kTitleChapterGap;
  const int blockH = std::max(0, std::min(DashboardMetrics::homeCoverImageHeight, textBottom - content.y - textH));
  const int statsW = std::min(content.width, isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth);
  const Rect coverArea{content.x, content.y, std::max(0, content.width - statsW - kCoverStatsGap), blockH};
  const Rect cover = CalibratedThemeLayout::fitCover(coverArea, DashboardMetrics::homeCoverImageWidth,
                                                     DashboardMetrics::homeCoverImageHeight, false);
  const int statsLeft = cover.x + cover.width + kCoverStatsGap;
  const Rect stats{statsLeft, content.y, std::max(0, right - statsLeft), blockH};
  const int textY = content.y + blockH + kTitleTopGap;
  return {cover, stats, Rect{content.x, textY, content.width, std::max(0, textBottom - textY)}, footer};
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

std::string coverPathForRect(const RecentBook& book, const Rect& coverRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    const std::string adaptivePath =
        Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(coverRect.width, coverRect.height);
    if (Storage.exists(adaptivePath.c_str())) {
      return adaptivePath;
    }
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, coverRect.width, coverRect.height, false);
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

  constexpr int iconSize = 32;
  drawLucideIcon(renderer, icon_book_open_32, coverRect.x + (coverRect.width - iconSize) / 2, coverRect.y + 36);

  constexpr int textPadding = 14;
  const int textW = coverRect.width - textPadding * 2;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int lineH = renderer.getLineHeight(renderer.filenameFontId(UI_12_FONT_ID));
  const int maxLines = std::max(0, std::min(4, (coverRect.height - 88) / lineH));
  if (maxLines == 0) return;
  auto titleLines =
      renderer.wrappedText(renderer.filenameFontId(UI_12_FONT_ID), title, textW, maxLines, EpdFontFamily::BOLD);
  int textY = coverRect.y + std::max(80, (coverRect.height - static_cast<int>(titleLines.size()) * lineH) / 2);
  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(renderer.filenameFontId(UI_12_FONT_ID), line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(renderer.filenameFontId(UI_12_FONT_ID), coverRect.x + (coverRect.width - lineW) / 2, textY,
                      line.c_str(), true, EpdFontFamily::BOLD);
    textY += lineH;
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const Rect bitmapRect = fittedBitmapRect(bitmap, coverRect);
        renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                 backgroundColor);
        renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, kCoverCornerRadius,
                                 Color::White);
        renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
        renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                               kCoverCornerRadius, backgroundColor);
        renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1, kCoverCornerRadius,
                                 true);
        hasCover = true;
      }
      file.close();
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}

void drawRightAlignedText(const GfxRenderer& renderer, const int fontId, const Rect& bounds, const int y,
                          const char* text, const bool bold = false, const bool black = true) {
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const auto visible = renderer.truncatedText(fontId, text, bounds.width, style);
  const int width = renderer.getTextWidth(fontId, visible.c_str(), style);
  renderer.drawText(fontId, bounds.x + bounds.width - width, y, visible.c_str(), black, style);
}

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}

bool fallbackEstimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalReadingSeconds < 120) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(stats.totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return fallbackEstimatedTimeLeft(stats, progressPercent, seconds);
}

bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }

  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) {
    return false;
  }

  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

int statsBlockHeight(const GfxRenderer& renderer) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  return valueLineH + kStatsValueLabelGap + labelLineH;
}

int statsBlockTop(const Rect& coverRect, const int index, const int blockH, const int rowCount) {
  const int remainingH = std::max(0, coverRect.height - blockH * rowCount);
  const int gapCount = rowCount - 1;
  const int gap = gapCount > 0 ? remainingH / gapCount : 0;
  const int remainder = gapCount > 0 ? remainingH % gapCount : 0;
  return coverRect.y + index * (blockH + gap) + std::min(index, remainder);
}

void drawStatsRow(const GfxRenderer& renderer, const Rect& bounds, const int y, const char* value, const char* label,
                  const bool black = true) {
  if (y < bounds.y || y + statsBlockHeight(renderer) > bounds.y + bounds.height) return;
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  drawRightAlignedText(renderer, UI_12_FONT_ID, bounds, y, value, true, black);
  drawRightAlignedText(renderer, SMALL_FONT_ID, bounds, y + valueLineH + kStatsValueLabelGap, label, false, black);
}

void drawDashboardStats(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                        const float progressPercent, const bool showStats, const bool black = true) {
  const int blockH = statsBlockHeight(renderer);
  const bool showRtcStats = halClock.isAvailable();
  const int rowCount = showRtcStats ? kStatsRowCount : kStatsRowCountX4;
  const BookReadingStats emptyStats{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : emptyStats;
  char value[40];
  char label[40];
  char startedDate[24];
  char finishDate[24];
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
  if (!showStats) {
    if (hasEstimate) {
      formatCompactDuration(estimatedSeconds, value, sizeof(value));
      drawStatsRow(renderer, coverRect, coverRect.y + coverRect.height / 2 - blockH, value, tr(STR_TIME_LEFT_SHORT),
                   black);
    }
    if (progressPercent >= 0.0f) {
      snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
      const int progressY = hasEstimate ? coverRect.y + coverRect.height / 2 + blockH : coverRect.y;
      drawStatsRow(renderer, coverRect, progressY, value, tr(STR_STATS_PROGRESS_LBL), black);
    }
    return;
  }
  ReadingStatsDateTime today;
  const bool hasToday = showRtcStats && getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = bookStats.isCompleted && bookStats.finishedDate.isValid()
                                       ? bookStats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = bookStats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(bookStats.startDate, endDate) : 0;

  int rowIndex = 0;
  int rowY = statsBlockTop(coverRect, rowIndex, blockH, rowCount);
  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, value, sizeof(value));
  drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_TIME_LBL), black);

  rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, coverRect, rowY, value, tr(STR_TIME_LEFT_SHORT), black);

  rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_PROGRESS_LBL), black);

  if (showRtcStats) {
    rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
    if (hasDaySpan) {
      const uint16_t dailyAverageDays = std::max<uint16_t>(1, daysReading);
      BookReadingStats::formatDuration(bookStats.totalReadingSeconds / dailyAverageDays, value, sizeof(value));
    } else {
      snprintf(value, sizeof(value), "-");
    }
    drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_DAILY_AVG_LBL), black);
  }

  rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
  snprintf(value, sizeof(value), "%.1f", pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));
  drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_PAGES_PER_MIN), black);

  if (!showRtcStats) {
    rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(bookStats.sessionCount));
    drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_SESSIONS_LBL), black);

    rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
    const uint32_t avgSeconds = bookStats.sessionCount > 0 ? bookStats.totalReadingSeconds / bookStats.sessionCount : 0;
    BookReadingStats::formatDuration(avgSeconds, value, sizeof(value));
    drawStatsRow(renderer, coverRect, rowY, value, tr(STR_STATS_AVG_SESSION_LBL), black);
    return;
  }

  rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
  if (hasDaySpan) {
    snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  formatReadingStatsShortDate(bookStats.startDate, startedDate, sizeof(startedDate));
  snprintf(label, sizeof(label), "%s %s", tr(STR_STATS_STARTED), startedDate);
  drawStatsRow(renderer, coverRect, rowY, value, label, black);

  rowY = statsBlockTop(coverRect, ++rowIndex, blockH, rowCount);
  ReadingStatsDate finishDisplayDate;
  if (bookStats.isCompleted) {
    finishDisplayDate = bookStats.finishedDate;
  } else if (hasToday && hasEstimate) {
    if (!estimateFinishDateFromDailyPace(bookStats, today, estimatedSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, estimatedSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, finishDate, sizeof(finishDate));
  drawStatsRow(renderer, coverRect, rowY, finishDate,
               bookStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE), black);
}

bool showBookStatsForPath(const std::string& path) {
  if (!SETTINGS.shouldTrackReadingStats()) return false;
  if (FsHelpers::hasEpubExtension(path))
    return BookStatsTracking::isBookEnabled(Epub::cachePathForFilePath(path, "/.crosspoint"));
  if (FsHelpers::hasXtcExtension(path))
    return BookStatsTracking::isBookEnabled(Xtc(path, "/.crosspoint").getCachePath());
  return false;
}

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

const char* readerTypeLabel(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return tr(STR_STATS_NEW_READER);
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
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

const uint8_t* readerTypeIcon(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return Book24Icon;
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return Book24Icon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
    default:
      return NightReaderIcon;
  }
}

void formatStreakStat(const GlobalReadingStats* globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }
  if (globalStats == nullptr) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats->currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }
  snprintf(buf, len, tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
}

void drawIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int iconX, const int centerY,
                   const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  if (inverted) {
    renderer.drawIconInverted(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  } else {
    renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  }
  renderer.drawText(UI_10_FONT_ID, iconX + kFooterIconSize + kFooterIconTextGap, centerY - lineH / 2,
                    visibleLabel.c_str(), !inverted);
}

void drawRightAlignedIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int rightX, const int centerY,
                               const char* label, const int maxTextW, const bool inverted = false) {
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textX = rightX - labelW;
  const int iconX = textX - kFooterIconTextGap - kFooterIconSize;
  if (inverted) {
    renderer.drawIconInverted(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  } else {
    renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  }
  renderer.drawText(UI_10_FONT_ID, textX, centerY - lineH / 2, visibleLabel.c_str(), !inverted);
}

void drawLeftAnchoredFooterStat(const GfxRenderer& renderer, const int labelX, const int centerY, const int maxTextW,
                                const char* value, const char* label, const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const auto visibleValue = renderer.truncatedText(UI_12_FONT_ID, value, maxTextW, EpdFontFamily::BOLD);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, visibleValue.c_str(), EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int topY = centerY - totalH / 2;
  const int blockW = std::max(labelW, valueW);
  renderer.drawText(UI_12_FONT_ID, labelX + (blockW - valueW) / 2, topY, visibleValue.c_str(), !inverted,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX + (blockW - labelW) / 2, topY + valueLineH + kStatsValueLabelGap,
                    visibleLabel.c_str(), !inverted);
}

void drawRightAnchoredFooterStat(const GfxRenderer& renderer, const int labelRightX, const int centerY,
                                 const int maxTextW, const char* value, const char* label,
                                 const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const auto visibleValue = renderer.truncatedText(UI_12_FONT_ID, value, maxTextW, EpdFontFamily::BOLD);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, visibleValue.c_str(), EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int labelX = labelRightX - std::max(labelW, valueW);
  const int topY = centerY - totalH / 2;
  const int blockW = std::max(labelW, valueW);
  renderer.drawText(UI_12_FONT_ID, labelX + (blockW - valueW) / 2, topY, visibleValue.c_str(), !inverted,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX + (blockW - labelW) / 2, topY + valueLineH + kStatsValueLabelGap,
                    visibleLabel.c_str(), !inverted);
}

void drawFooterStats(const GfxRenderer& renderer, const Rect& footer, const GlobalReadingStats* globalStats,
                     const bool inverted = false) {
  const int centerY = footer.y + footer.height / 2;
  const int halfW = footer.width / 2;
  constexpr int pairGap = 12;

  if (!halClock.isAvailable()) {
    char totalTime[40];
    char booksRead[16];
    const uint32_t totalReadingSeconds = globalStats != nullptr ? globalStats->totalReadingSeconds : 0;
    const uint32_t completedBooks = globalStats != nullptr ? globalStats->completedBooks : 0;
    BookReadingStats::formatDuration(totalReadingSeconds, totalTime, sizeof(totalTime));
    snprintf(booksRead, sizeof(booksRead), "%lu", static_cast<unsigned long>(completedBooks));

    const int maxTextW = std::max(1, halfW - pairGap);
    drawLeftAnchoredFooterStat(renderer, footer.x, centerY, maxTextW, totalTime,
                               tr(STR_STATS_TOTAL_READING_TIME_LBL_SHORT), inverted);
    const int rightX = footer.x + footer.width;
    drawRightAnchoredFooterStat(renderer, rightX, centerY, maxTextW, booksRead, tr(STR_STATS_COMPLETED_LBL), inverted);
    return;
  }

  char streakBuf[48];
  formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));

  const int leftTextW = halfW - pairGap - kFooterIconSize - kFooterIconTextGap;
  drawIconLabel(renderer, StreakIcon, footer.x, centerY, streakBuf, leftTextW, inverted);

  const char* readerLabel = readerTypeLabel(globalStats);
  const int rightX = footer.x + footer.width;
  const int maxReaderTextW = std::max(1, halfW - pairGap - kFooterIconSize - kFooterIconTextGap);
  drawRightAlignedIconLabel(renderer, readerTypeIcon(globalStats), rightX, centerY, readerLabel, maxReaderTextW,
                            inverted);
}

void drawBookText(const GfxRenderer& renderer, const Rect& text, const RecentBook& book,
                  const char* currentChapterTitle, const bool black = true) {
  const int textW = text.width;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(renderer.filenameFontId(UI_12_FONT_ID), title, textW, kBookTitleMaxLines,
                                         EpdFontFamily::BOLD);
  int textY = text.y;
  const int titleLineH = renderer.getLineHeight(renderer.filenameFontId(UI_12_FONT_ID));
  for (const auto& line : titleLines) {
    if (textY + titleLineH > text.y + text.height) break;
    renderer.drawText(renderer.filenameFontId(UI_12_FONT_ID), text.x, textY, line.c_str(), black, EpdFontFamily::BOLD);
    textY += titleLineH;
  }

  const char* subtitle =
      (currentChapterTitle != nullptr && currentChapterTitle[0] != '\0') ? currentChapterTitle : book.author.c_str();
  if (subtitle != nullptr && subtitle[0] != '\0') {
    auto subtitleLines =
        renderer.wrappedText(renderer.filenameFontId(UI_12_FONT_ID), subtitle, textW, kBookChapterMaxLines);
    int subtitleY = textY + kTitleChapterGap;
    for (const auto& line : subtitleLines) {
      if (subtitleY + titleLineH > text.y + text.height) break;
      renderer.drawText(renderer.filenameFontId(UI_12_FONT_ID), text.x, subtitleY, line.c_str(), black);
      subtitleY += titleLineH;
    }
  }
}
}  // namespace

Rect DashboardTheme::getHomeCoverRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int top = metrics.homeTopPadding + UITheme::getHeaderRect(renderer, 0).y - metrics.topPadding;
  return layoutForContent(renderer, CalibratedThemeLayout::content(
                                        renderer, Rect{safe.x, top, safe.width, metrics.homeCoverTileHeight}, true))
      .cover;
}

Rect DashboardTheme::getSleepCoverRect(const GfxRenderer& renderer) {
  return layoutForContent(renderer, CalibratedThemeLayout::sleep(renderer, DashboardMetrics::values.homeTopPadding))
      .cover;
}

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                         const BookReadingStats* stats, const float progressPercent,
                                         const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)selectorIndex;
  (void)bufferRestored;

  const auto layout = layoutForContent(renderer, CalibratedThemeLayout::content(renderer, rect, true));
  const Rect& coverRect = layout.cover;
  if (coverRect.width <= 0 || coverRect.height <= 0) return;
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  TouchRegistry::getInstance().add(coverRect, 0, TouchRegistry::Cover);

  drawDashboardStats(renderer, layout.stats, stats, progressPercent, showBookStatsForPath(recentBooks[0].path));
  drawBookText(renderer, layout.text, recentBooks[0], currentChapterTitle);
  if (SETTINGS.shouldTrackReadingStats()) drawFooterStats(renderer, layout.footer, globalStats);
}

void DashboardTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                     const GlobalReadingStats* globalStats, const float progressPercent,
                                     const char* currentChapterTitle, const bool inverted) const {
  renderer.clearScreen(inverted ? 0xFF : 0x00);

  const auto layout =
      layoutForContent(renderer, CalibratedThemeLayout::sleep(renderer, DashboardMetrics::values.homeTopPadding));
  const Rect& coverRect = layout.cover;
  if (coverRect.width <= 0 || coverRect.height <= 0) return;
  drawBookCover(renderer, coverRect, book, inverted ? Color::White : Color::Black);
  drawDashboardStats(renderer, layout.stats, stats, progressPercent, showBookStatsForPath(book.path), inverted);
  drawBookText(renderer, layout.text, book, currentChapterTitle, inverted);
  if (SETTINGS.shouldTrackReadingStats()) drawFooterStats(renderer, layout.footer, globalStats, !inverted);
}
