#pragma once

#include <string>

struct RecentBook;

// Helpers for loading and displaying recent-book reading progress.
// Progress values are percentages in the 0-100 range; negative means unknown.
//
// Example:
//   const float progress = RecentBookProgress::loadPercent(book);
//   const std::string label = RecentBookProgress::formatPercent(progress);
namespace RecentBookProgress {
// Loads the saved reading percentage for a recent EPUB, XTC, TXT, or Markdown book.
// Returns -1.0f when progress is unavailable or the cache cannot be read.
float loadPercent(const RecentBook& book);
// Returns true when progress contains a known 0-100 percentage.
bool hasPercent(float progress);
// Formats progress as a rounded percentage string such as "42%".
// Returns an empty string when progress is unknown.
std::string formatPercent(float progress);
}  // namespace RecentBookProgress
