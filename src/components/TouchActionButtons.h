#pragma once

#include <cstddef>
#include <cstdint>

#include "themes/BaseTheme.h"

class GfxRenderer;

// Fixed-capacity layout and drawing helpers for touch-only action groups. The
// same Layout must be used by rendering and hit testing so translated labels
// cannot make the two paths drift apart.
namespace TouchActionButtons {

constexpr size_t kMaxButtons = 4;
constexpr int kDefaultHeight = 56;
constexpr int kDefaultGap = 16;

struct Layout {
  Rect container;
  Rect buttons[kMaxButtons];
  uint8_t count = 0;

  Layout() : container(), buttons{Rect(), Rect(), Rect(), Rect()} {}

  const Rect* data() const { return buttons; }
};

Layout vertical(Rect container, uint8_t count, int buttonHeight = kDefaultHeight, int gap = kDefaultGap);

// Optional finger-placement margin; callers should keep it within half the gap between buttons.
int indexAt(const Layout& layout, int x, int y, int touchPadding = 0);

void draw(const GfxRenderer& renderer, const Layout& layout, const char* const* labels, int primaryIndex = -1,
          int selectedIndex = -1, int fontId = 0);

}  // namespace TouchActionButtons
