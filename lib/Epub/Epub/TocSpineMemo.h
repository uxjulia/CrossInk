#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

// One adjacent-TOC-entry memo for the small-spine linear fallback. Paths are
// already separated from anchors by the TOC parser. Only exact, successful
// lookups are cached; long paths and transient misses retain the original path.
class TocSpineMemo {
 public:
  void clear() { spineIndex = -1; }

  template <typename Search>
  int16_t lookup(const std::string_view href, Search&& search) {
    if (spineIndex >= 0 && href == std::string_view(lastHref.data(), lastLength)) return spineIndex;
    const int16_t found = search();
    if (found >= 0 && href.size() <= lastHref.size()) {
      std::copy(href.begin(), href.end(), lastHref.begin());
      lastLength = static_cast<uint8_t>(href.size());
      spineIndex = found;
    } else {
      clear();
    }
    return found;
  }

 private:
  std::array<char, 64> lastHref{};
  uint8_t lastLength = 0;
  int16_t spineIndex = -1;
};
