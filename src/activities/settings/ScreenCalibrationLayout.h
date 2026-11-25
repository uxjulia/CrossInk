#pragma once

#include <algorithm>

// Fixed preview lanes keep controls still while draft edge values change.
struct ScreenCalibrationLayout {
  static constexpr int CONTROL_COUNT = 6;
  static constexpr int ROW_GAP = 10;
  static constexpr int SECTION_GAP = 12;
  int helpY;
  int helpHeight;
  int panelY;
  int rowHeight;
  int stepSize;

  static ScreenCalibrationLayout fit(int height, int lineHeight, int previewReserve, int hintReserve, bool touch) {
    const int helpY = previewReserve + lineHeight + 8;
    const int helpHeight = lineHeight * 2;
    const int panelY = helpY + helpHeight + SECTION_GAP;
    const int bottom = height - std::max(previewReserve, hintReserve + 8);
    const int noteHeight = touch ? 0 : 3 * (lineHeight + 4) + SECTION_GAP;
    // Six adjustment rows and two action rows share all remaining height.
    const int rowHeight = std::max(1, (bottom - panelY - noteHeight - 6 * ROW_GAP - SECTION_GAP) / 8);
    return {helpY, helpHeight, panelY, rowHeight, std::max(1, std::min(rowHeight - 8, 72))};
  }
};
