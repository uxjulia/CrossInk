#pragma once

#include <algorithm>
#include <array>

#include "ScreenInsets.h"

// CrossInk UI clearance policy, separate from user-adjustable straight edges.
struct PanelLayout {
  int headerOffsetY = 0;
  int statusBarOffsetY = 0;
  bool statusBarAtCalibratedEdge = false;
  int tabBarOffsetY = 0;
  int tabBarHeightPct = 100;
  int tabBarWidthPct = 100;

  int topStatusBarY(const ScreenInsets& insets, unsigned orientation, int legacyOrigin) const {
    return statusBarAtCalibratedEdge ? insets.rotated(orientation).edges[0]
                                     : insets.topOrigin(orientation, legacyOrigin);
  }

  // Apply the board's tab proportions without moving controls under the bezel.
  std::array<int, 4> tabs(int x, int y, int width, int height, int safeBottom) const {
    const int fittedWidth = width * std::clamp(tabBarWidthPct, 1, 100) / 100;
    const int fittedHeight = height * std::clamp(tabBarHeightPct, 1, 100) / 100;
    return {{x + (width - fittedWidth) / 2,
             std::min(y + height - fittedHeight + tabBarOffsetY, safeBottom - fittedHeight), fittedWidth,
             fittedHeight}};
  }
};
