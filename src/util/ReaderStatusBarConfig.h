#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>

enum class ReaderStatusBarPosition : uint8_t { Top, Bottom };

constexpr ReaderStatusBarPosition xtcStatusBarConfigPosition(const ReaderStatusBarPosition displayed,
                                                             const bool legacyTopUsesBottom) {
  return displayed == ReaderStatusBarPosition::Top && legacyTopUsesBottom ? ReaderStatusBarPosition::Bottom : displayed;
}

// The stored values are deliberately independent of the order shown in the picker.
enum class ReaderStatusBarItem : uint8_t {
  Empty = 0,
  Clock,
  Battery,
  TimeLeftBook,
  TimeLeftChapter,
  ChapterPageCount,
  StablePageNumber,
  BookProgressPercentage,
  TitleBook,
  TitleChapter,
  Date,
  ChapterProgressPercentage,
  Count,
};

constexpr bool validReaderStatusBarItemValue(const int value, const bool clockAvailable) {
  return value >= 0 && value < static_cast<int>(ReaderStatusBarItem::Count) &&
         ((value != static_cast<int>(ReaderStatusBarItem::Clock) &&
           value != static_cast<int>(ReaderStatusBarItem::Date)) ||
          clockAvailable);
}

// How a Battery slot renders. Values are persisted; keep them stable.
enum class ReaderStatusBarBatteryStyle : uint8_t { IconAndPercent = 0, IconOnly, PercentOnly, Count };

// Glyph joining items when a bar is centered. Values are persisted; keep them stable.
enum class ReaderStatusBarSeparator : uint8_t { Bullet = 0, Dash, Pipe, MiddleDot, Count };

// Flash-resident glyphs: bullet U+2022, en dash U+2013, vertical bar, middle dot U+00B7.
inline const char* readerStatusBarSeparatorGlyph(const ReaderStatusBarSeparator separator) {
  static const char* const GLYPHS[] = {"\xE2\x80\xA2", "\xE2\x80\x93", "|", "\xC2\xB7"};
  const auto index = static_cast<unsigned>(separator);
  return index < sizeof(GLYPHS) / sizeof(GLYPHS[0]) ? GLYPHS[index] : GLYPHS[0];
}

constexpr bool validReaderStatusBarChoice(const int value, const int optionCount) {
  return value >= 0 && value < optionCount;
}

struct ReaderStatusBarConfig {
  static constexpr int TOP_TEXT_INSET = 2;
  static constexpr int BOOKMARK_WIDTH = 9;
  static constexpr int BOOKMARK_HEIGHT = 14;
  static constexpr unsigned SLOT_COUNT = 7;
  static constexpr unsigned LEFT_FIRST = 0;
  static constexpr unsigned LEFT_SECOND = 1;
  static constexpr unsigned LEFT_THIRD = 2;
  static constexpr unsigned CENTER = 3;
  static constexpr unsigned RIGHT_FIRST = 4;
  static constexpr unsigned RIGHT_SECOND = 5;
  static constexpr unsigned RIGHT_THIRD = 6;

  std::array<ReaderStatusBarItem, SLOT_COUNT> slots{};
  uint8_t percentageFormat = 0;
  uint8_t progressBar = 2;
  uint8_t progressBarThickness = 1;
  ReaderStatusBarBatteryStyle batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;
  bool hidden = false;
  bool centered = false;
  ReaderStatusBarSeparator separator = ReaderStatusBarSeparator::Bullet;

  constexpr bool contains(ReaderStatusBarItem item) const {
    return std::any_of(slots.begin(), slots.end(), [item](const auto slot) { return slot == item; });
  }

  constexpr bool hasTextItems(bool clockAvailable) const {
    return std::any_of(slots.begin(), slots.end(), [clockAvailable](const auto slot) {
      return slot != ReaderStatusBarItem::Empty &&
             ((slot != ReaderStatusBarItem::Clock && slot != ReaderStatusBarItem::Date) || clockAvailable);
    });
  }
};

inline ReaderStatusBarConfig xtcStatusBarConfigForDisplay(const ReaderStatusBarPosition displayed,
                                                          const bool legacyTopUsesBottom,
                                                          const ReaderStatusBarConfig& top,
                                                          const ReaderStatusBarConfig& bottom) {
  auto config =
      xtcStatusBarConfigPosition(displayed, legacyTopUsesBottom) == ReaderStatusBarPosition::Top ? top : bottom;
  config.hidden = displayed == ReaderStatusBarPosition::Top ? top.hidden : bottom.hidden;
  return config;
}

constexpr bool validDisplayStatusBarItemValue(const int value, const bool clockAvailable) {
  return value == static_cast<int>(ReaderStatusBarItem::Empty) ||
         (clockAvailable && (value == static_cast<int>(ReaderStatusBarItem::Clock) ||
                             value == static_cast<int>(ReaderStatusBarItem::Date))) ||
         value == static_cast<int>(ReaderStatusBarItem::Battery);
}

struct DisplayStatusBarConfig {
  std::array<ReaderStatusBarItem, 3> slots{ReaderStatusBarItem::Empty, ReaderStatusBarItem::Empty,
                                           ReaderStatusBarItem::Battery};
  ReaderStatusBarBatteryStyle batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;

  ReaderStatusBarConfig asReaderConfig() const {
    ReaderStatusBarConfig config;
    config.progressBar = 2;
    config.batteryStyle = batteryStyle;
    config.slots[ReaderStatusBarConfig::LEFT_FIRST] = slots[0];
    config.slots[ReaderStatusBarConfig::CENTER] = slots[1];
    config.slots[ReaderStatusBarConfig::RIGHT_FIRST] = slots[2];
    return config;
  }
};

struct ReaderStatusBarsPayload {
  ReaderStatusBarConfig top;
  ReaderStatusBarConfig bottom;
  uint8_t xtcMode = 0;
};

constexpr int readerStatusBarTotalHeight(const ReaderStatusBarPosition position, const bool hasText,
                                         const int progressSpace, const int textLaneHeight) {
  if (!hasText && progressSpace > 0 && position == ReaderStatusBarPosition::Bottom) {
    return std::max(progressSpace, ReaderStatusBarConfig::BOOKMARK_HEIGHT);
  }
  return (hasText
              ? textLaneHeight + (position == ReaderStatusBarPosition::Top ? ReaderStatusBarConfig::TOP_TEXT_INSET : 0)
              : 0) +
         progressSpace;
}

struct LegacyReaderStatusBarSettings {
  bool chapterPageCount = false;
  bool stablePageNumbers = false;
  bool bookProgressPercentage = false;
  uint8_t titleMode = 2;
  uint8_t timeLeftMode = 0;
  bool battery = false;
  uint8_t percentageFormat = 0;
  uint8_t progressBar = 2;
  uint8_t progressBarThickness = 1;
};

constexpr ReaderStatusBarConfig migrateBottomStatusBar(const LegacyReaderStatusBarSettings& old) {
  ReaderStatusBarConfig result;
  result.percentageFormat = old.percentageFormat;
  result.progressBar = old.progressBar;
  result.progressBarThickness = old.progressBarThickness;
  unsigned left = ReaderStatusBarConfig::LEFT_FIRST;
  unsigned right = ReaderStatusBarConfig::RIGHT_FIRST;
  if (old.battery) result.slots[left++] = ReaderStatusBarItem::Battery;
  if (old.timeLeftMode == 1) result.slots[left++] = ReaderStatusBarItem::TimeLeftChapter;
  if (old.timeLeftMode == 2) result.slots[left++] = ReaderStatusBarItem::TimeLeftBook;
  if (old.titleMode == 0) result.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::TitleBook;
  if (old.titleMode == 1) result.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::TitleChapter;
  if (old.chapterPageCount) result.slots[right++] = ReaderStatusBarItem::ChapterPageCount;
  if (old.stablePageNumbers) result.slots[right++] = ReaderStatusBarItem::StablePageNumber;
  if (old.bookProgressPercentage) result.slots[right++] = ReaderStatusBarItem::BookProgressPercentage;
  return result;
}

struct ReaderStatusBarGeometry {
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> x{};
  int centerLeft = 0;
  int centerRight = 0;
};

// Keep short edge items at their natural width. When the edge clusters would
// overlap, trim the longest items until both clusters fit the available row.
inline void fitReaderStatusBarSideWidths(std::array<int, ReaderStatusBarConfig::SLOT_COUNT>& widths,
                                         const int available, const int bookmarkReserve = 0, const int gap = 8) {
  int leftCount = 0;
  int rightCount = 0;
  for (unsigned i = ReaderStatusBarConfig::LEFT_FIRST; i <= ReaderStatusBarConfig::LEFT_THIRD; ++i) {
    if (widths[i] > 0) ++leftCount;
  }
  for (unsigned i = ReaderStatusBarConfig::RIGHT_FIRST; i <= ReaderStatusBarConfig::RIGHT_THIRD; ++i) {
    if (widths[i] > 0) ++rightCount;
  }
  if (leftCount + rightCount == 0) return;
  const int gaps =
      std::max(0, leftCount - 1) + std::max(0, rightCount - 1) + (bookmarkReserve > 0 && leftCount > 0 ? 1 : 0);
  const int budget = std::max(0, available - bookmarkReserve - gaps * gap);
  int low = 0;
  int high = budget;
  while (low < high) {
    const int cap = low + (high - low + 1) / 2;
    int used = 0;
    for (unsigned i = 0; i < ReaderStatusBarConfig::SLOT_COUNT; ++i) {
      if (i == ReaderStatusBarConfig::CENTER) continue;
      used += std::min(widths[i], cap);
    }
    if (used <= budget)
      low = cap;
    else
      high = cap - 1;
  }
  for (unsigned i = 0; i < ReaderStatusBarConfig::SLOT_COUNT; ++i) {
    if (i != ReaderStatusBarConfig::CENTER) widths[i] = std::min(widths[i], low);
  }
}

inline ReaderStatusBarGeometry layoutReaderStatusBarItems(
    const int leftEdge, const int rightEdge, const std::array<int, ReaderStatusBarConfig::SLOT_COUNT>& widths,
    const int bookmarkReserve = 0, const int gap = 8) {
  ReaderStatusBarGeometry result;
  int leftX = leftEdge + bookmarkReserve;
  bool hasLeft = bookmarkReserve > 0;
  for (unsigned i = ReaderStatusBarConfig::LEFT_FIRST; i <= ReaderStatusBarConfig::LEFT_THIRD; ++i) {
    if (widths[i] <= 0) continue;
    if (hasLeft) leftX += gap;
    result.x[i] = leftX;
    leftX += widths[i];
    hasLeft = true;
  }

  int rightWidth = 0;
  for (unsigned i = ReaderStatusBarConfig::RIGHT_FIRST; i <= ReaderStatusBarConfig::RIGHT_THIRD; ++i) {
    if (widths[i] <= 0) continue;
    if (rightWidth > 0) rightWidth += gap;
    rightWidth += widths[i];
  }
  int rightX = rightEdge - rightWidth;
  for (unsigned i = ReaderStatusBarConfig::RIGHT_FIRST; i <= ReaderStatusBarConfig::RIGHT_THIRD; ++i) {
    if (widths[i] <= 0) continue;
    result.x[i] = rightX;
    rightX += widths[i] + gap;
  }
  result.centerLeft = std::clamp(leftX + (hasLeft ? gap : 0), leftEdge, rightEdge);
  result.centerRight = std::clamp(rightEdge - rightWidth - (rightWidth > 0 ? gap : 0), result.centerLeft, rightEdge);
  return result;
}

struct CenteredReaderStatusBarGeometry {
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> x{};
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{};  // Trimmed widths.
  // Separator i sits between the i-th and (i+1)-th drawn items, in slot order.
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT - 1> separatorX{};
  int separatorCount = 0;
  int totalWidth = 0;
};

// Lays out every non-empty slot as one group centered on centerX, with a separator
// between consecutive items. Overflow trims the widest items first.
inline CenteredReaderStatusBarGeometry layoutCenteredReaderStatusBarItems(
    const int leftEdge, const int rightEdge, const int centerX,
    const std::array<int, ReaderStatusBarConfig::SLOT_COUNT>& widths, const int separatorWidth,
    const int bookmarkReserve = 0, const int separatorGap = 6) {
  CenteredReaderStatusBarGeometry result;
  result.widths = widths;
  int count = 0;
  int sum = 0;
  for (auto& w : result.widths) {
    if (w < 0) w = 0;
    if (w > 0) ++count;
    sum += w;
  }
  if (count == 0) return result;

  const int joinCost = separatorWidth + 2 * separatorGap;
  const int minX = leftEdge + std::max(0, bookmarkReserve);
  const int available = std::max(0, rightEdge - minX);
  const int budget = std::max(0, available - (count - 1) * joinCost);
  if (sum > budget) {
    int low = 0;
    int high = budget;
    while (low < high) {
      const int cap = low + (high - low + 1) / 2;
      const int used = std::accumulate(result.widths.begin(), result.widths.end(), 0,
                                       [cap](int total, int width) { return total + std::min(width, cap); });
      if (used <= budget)
        low = cap;
      else
        high = cap - 1;
    }
    sum = 0;
    for (auto& w : result.widths) {
      w = std::min(w, low);
      sum += w;
    }
  }

  const int total = (count - 1) * joinCost + sum;
  result.totalWidth = total;
  int cursor = std::max(minX, std::min(centerX - total / 2, rightEdge - total));
  bool placedAny = false;
  for (unsigned i = 0; i < ReaderStatusBarConfig::SLOT_COUNT; ++i) {
    if (result.widths[i] <= 0) continue;
    if (placedAny) {
      result.separatorX[result.separatorCount++] = cursor + separatorGap;
      cursor += joinCost;
    }
    result.x[i] = cursor;
    cursor += result.widths[i];
    placedAny = true;
  }
  return result;
}
