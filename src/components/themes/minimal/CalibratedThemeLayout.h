#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"

// Shared geometry for the Minimal and Dashboard home/sleep artwork. Calibration
// changes the available rectangle, never the bitmap cache identity.
namespace CalibratedThemeLayout {
inline Rect content(const GfxRenderer& renderer, const Rect& requested, const bool buttonHints) {
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, buttonHints, false);
  const int x = std::max(safe.x, requested.x);
  const int y = std::max(safe.y, requested.y);
  return Rect{x, y, std::max(0, std::min(safe.x + safe.width, requested.x + requested.width) - x),
              std::max(0, std::min(safe.y + safe.height, requested.y + requested.height) - y)};
}

inline Rect sleep(const GfxRenderer& renderer, const int topPadding) {
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer);
  const int top = renderer.getViewableInsets().topOrigin(static_cast<unsigned>(renderer.getOrientation()), topPadding);
  return content(renderer, Rect{safe.x, top, safe.width, std::max(0, safe.y + safe.height - top)}, false);
}

inline Rect fitCover(const Rect& area, const int width, const int height, const bool centered) {
  int fittedW = std::min(width, area.width);
  int fittedH = fittedW * height / width;
  if (fittedH > area.height) {
    fittedH = area.height;
    fittedW = fittedH * width / height;
  }
  return Rect{area.x + (centered ? (area.width - fittedW) / 2 : 0), area.y, fittedW, fittedH};
}
}  // namespace CalibratedThemeLayout
