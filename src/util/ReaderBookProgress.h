#pragma once

#include <I18n.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// The reader menu and the frontlight panel show the same compact progress line.
// A zero page count means that the current format has no chapter page count.
inline void formatReaderBookProgress(char* buffer, size_t size, uint32_t chapterPage, uint32_t chapterPageCount,
                                     bool chapterPageCountEstimated, int bookPercent) {
  if (size == 0) return;
  const int percent = std::clamp(bookPercent, 0, 100);
  if (chapterPageCount > 0) {
    std::snprintf(buffer, size, "%s: %s%lu/%lu%s%s: %d%%", tr(STR_CHAPTER), chapterPageCountEstimated ? "~" : "",
                  static_cast<unsigned long>(std::min(chapterPage, chapterPageCount)),
                  static_cast<unsigned long>(chapterPageCount), tr(STR_PAGES_SEPARATOR), tr(STR_BOOK), percent);
  } else {
    std::snprintf(buffer, size, "%s: %d%%", tr(STR_BOOK), percent);
  }
}
