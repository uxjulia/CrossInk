#pragma once

#include <algorithm>

// Coordinates shared by the legacy interval picker's drawing and touch routing.
// Optional step controls disappear if they cannot fit; physical buttons and the
// slider still adjust the value. Never move content back into the header.
struct IntervalSelectionLayout {
  static constexpr int barHeight = 16;
  static constexpr int knobOverhang = 4;
  int valueY = 0;
  int barY = 0;
  int stepsY = 0;
  bool visible = false;
  bool showSteps = false;
};

inline IntervalSelectionLayout intervalSelectionLayout(const int top, const int bottom, const int valueHeight,
                                                       const int stepsHeight, const int gap, const bool bottomAligned) {
  IntervalSelectionLayout layout;
  const int spacing = std::max(4, gap);
  const int available = std::max(0, bottom - top);
  const int sliderHeight = IntervalSelectionLayout::barHeight + 2 * IntervalSelectionLayout::knobOverhang;
  const int minimumHeight = valueHeight + spacing + sliderHeight;
  if (valueHeight <= 0 || available < minimumHeight) return layout;
  layout.visible = true;
  layout.showSteps = stepsHeight > 0 && available >= minimumHeight + spacing + stepsHeight;
  const int height = minimumHeight + (layout.showSteps ? spacing + stepsHeight : 0);
  layout.valueY = top + (bottomAligned ? available - height : (available - height) / 2);
  layout.barY = layout.valueY + valueHeight + spacing + IntervalSelectionLayout::knobOverhang;
  layout.stepsY = layout.barY + IntervalSelectionLayout::barHeight + IntervalSelectionLayout::knobOverhang + spacing;
  return layout;
}
