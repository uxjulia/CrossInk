#pragma once

#include <I18n.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct ReaderTimeLeftLabels {
  char book[24]{};
  char chapter[24]{};

  bool empty() const { return !book[0] && !chapter[0]; }
};

inline void appendReaderTimeLeft(char* buffer, size_t size, const ReaderTimeLeftLabels& labels) {
  if (size == 0) return;
  const auto append = [&](const char* duration, const char* scope) {
    if (!duration[0]) return;
    const size_t used = std::strlen(buffer);
    // Append complete fields only, including the separator.
    const char* separator = used > 0 ? "  |  " : "";
    const size_t needed = std::strlen(separator) + std::strlen(duration) + std::strlen(scope) + sizeof(" ()");
    if (needed <= size - used) {
      std::snprintf(buffer + used, size - used, "%s%s (%s)", separator, duration, scope);
    }
  };
  append(labels.book, tr(STR_BOOK));
  append(labels.chapter, tr(STR_CHAPTER));
}

// The reader menu and the frontlight panel show the same compact progress line.
// A zero page count means that the current format has no chapter page count.
inline void formatReaderBookProgress(char* buffer, size_t size, uint32_t chapterPage, uint32_t chapterPageCount,
                                     bool chapterPageCountEstimated, int bookPercent,
                                     const ReaderTimeLeftLabels& timeLeft = {}) {
  if (size == 0) return;
  const int percent = std::clamp(bookPercent, 0, 100);
  if (chapterPageCount > 0) {
    std::snprintf(buffer, size, "%s: %s%lu/%lu%s%s: %d%%", tr(STR_CHAPTER), chapterPageCountEstimated ? "~" : "",
                  static_cast<unsigned long>(std::min(chapterPage, chapterPageCount)),
                  static_cast<unsigned long>(chapterPageCount), tr(STR_PAGES_SEPARATOR), tr(STR_BOOK), percent);
  } else {
    std::snprintf(buffer, size, "%s: %d%%", tr(STR_BOOK), percent);
  }
  appendReaderTimeLeft(buffer, size, timeLeft);
}
