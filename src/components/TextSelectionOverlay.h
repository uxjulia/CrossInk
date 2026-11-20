#pragma once

#include <AppCapabilities.h>
#if CROSSINK_APP_CAP_TOUCH
#include <FreeInkUICore.h>

class GfxRenderer;

// Reader-specific placement; the SDK owns button paint and rectangle hit testing.
namespace TextSelectionOverlay {
using Rect = freeink::ui::Rect;
constexpr int actionCount = 2;
constexpr int lookupAction = 0;
constexpr int clipAction = 1;

// A drag handle: a stem along the selection edge plus a dot past the line
// (above the start, below the end). `hit` is the larger touch area.
struct Handle {
  Rect stem;
  Rect dot;
  Rect hit;
  bool empty() const { return hit.empty(); }
};

struct Layout {
  // Indexed by lookupAction/clipAction. An empty rect means the action is hidden.
  Rect actions[actionCount];
  Rect bar;
  int16_t pointerX = 0;
  bool pointerUp = false;  // The bar sits below the selection.
  bool showPointer = false;
  bool showIcons = true;
  Handle start;
  Handle end;
};

Rect safeArea(const GfxRenderer& renderer);
// `word` is the selected word's line box; `onLeft` puts the handle on its left edge.
Handle handle(Rect safe, Rect word, bool onLeft, bool isStart);
const Handle* handleAt(const Layout& layout, int x, int y);
// Places the toolbar from the selection's first and last word boxes (either may
// be empty) and the handles already stored in `layout`:
//   1. above the selection when it fits, pointing down at the first word;
//   2. otherwise below the selection, pointing up at the last word;
//   3. otherwise at the screen edge that covers the least of the selection,
//      shifted sideways if needed so it never hides a drag handle.
void placeActions(const GfxRenderer& renderer, Layout& layout, Rect firstWord, Rect lastWord, bool lookupAvailable);
int actionAt(const Layout& layout, int x, int y);
void draw(const GfxRenderer& renderer, const Layout& layout, bool showActions);
}  // namespace TextSelectionOverlay
#endif
