#pragma once

#include <algorithm>

// Keeps the status-bar tap region independent from reader layout and input so
// it can be host-tested. Reserve a finger-sized strip plus the bezel margin
// at each enabled bar, including while the bars are temporarily hidden.
class ReaderStatusBarTapTarget {
 public:
  static constexpr int MIN_HEIGHT_PX = 64;

  static bool containsBottom(const int y, const int screenHeight, const int orientedBottomMargin,
                             const int statusBarHeight) {
    if (screenHeight <= 0 || orientedBottomMargin < 0 || statusBarHeight <= 0) {
      return false;
    }

    const int bottom = screenHeight - orientedBottomMargin;
    if (bottom <= 0) {
      return false;
    }
    const int height = std::min(bottom, std::max(MIN_HEIGHT_PX, statusBarHeight));
    return y >= bottom - height && y < screenHeight;
  }

  static bool containsTop(const int y, const int screenHeight, const int orientedTopMargin, const int statusBarHeight) {
    if (screenHeight <= 0 || orientedTopMargin < 0 || statusBarHeight <= 0 || orientedTopMargin >= screenHeight) {
      return false;
    }

    const int height = std::min(screenHeight - orientedTopMargin, std::max(MIN_HEIGHT_PX, statusBarHeight));
    return y >= 0 && y < orientedTopMargin + height;
  }
};
