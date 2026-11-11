#pragma once

#include <FreeInkUIGfxRenderer.h>

#include "GfxRenderer.h"
#include "Icon.h"
#include "MappedInputManager.h"
#include "themes/BaseTheme.h"

namespace TouchHeaderBackButton {

constexpr int ICON_SIZE = 32;
// Keep navigation controls close to the divider while leaving the status row unchanged.
constexpr int TITLE_VERTICAL_OFFSET = 8;

struct Layout {
  Rect iconRect;
  Rect touchRect;
  int titleX;
};

Layout layout(const Rect& header);
int height(const ThemeMetrics& metrics, const MappedInputManager& input);
Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input);
Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input, const Rect& area);
Rect standardHeaderRect(const GfxRenderer& renderer);
Rect compactHeaderRect(const GfxRenderer& renderer);
int contentTop(const GfxRenderer& renderer, const MappedInputManager& input, int areaTop = 0);
bool wasTapped(const MappedInputManager& input, const Rect& header);
bool wasTapped(const MappedInputManager& input, const GfxRenderer& renderer);
// Include the heading in the Back target without measuring text between taps.
bool wasTapped(const MappedInputManager& input, const GfxRenderer& renderer, const char* title);
void draw(GfxRenderer& renderer, const Rect& header, const char* title, bool readerContext, int rightReserve = 0,
          const char* subtitle = nullptr, int verticalOffset = TITLE_VERTICAL_OFFSET, bool showStatus = true);
void draw(GfxRenderer& renderer, freeink::ui::GfxRendererTarget& target, const Rect& header, const char* title,
          bool readerContext, int rightReserve = 0, const char* subtitle = nullptr,
          int verticalOffset = TITLE_VERTICAL_OFFSET, bool showStatus = true);
// minRightReserve keeps the title clear of a trailing action (see TRAILING_ACTION_RESERVE).
void drawCompact(GfxRenderer& renderer, const char* title, bool readerContext = false, bool showDate = false,
                 int verticalOffset = TITLE_VERTICAL_OFFSET, int minRightReserve = 0);

// A trailing header action mirrors the back button: same lane and touch target,
// on the right edge of the back button's row.
extern const int TRAILING_ACTION_RESERVE;
void drawTrailingIcon(GfxRenderer& renderer, const Rect& header, const freeink::Icon& icon,
                      int verticalOffset = TITLE_VERTICAL_OFFSET);
Rect trailingTouchRect(const Rect& header);

}  // namespace TouchHeaderBackButton
