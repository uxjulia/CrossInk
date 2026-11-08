#pragma once

#include <string>

// A book is enabled by default. The marker is separate from stats_v7.bin so
// clearing statistics or EPUB reader settings does not reset the user's choice.
namespace BookStatsTracking {
bool isBookEnabled(const std::string& cachePath);
bool isEnabled(const std::string& cachePath);
bool setBookEnabled(const std::string& cachePath, bool enabled);
}  // namespace BookStatsTracking
