#include "BookLoadingFeedback.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"

bool BookLoadingFeedback::showIfPending(GfxRenderer& renderer) {
  if (!renderer.hasFrameBuffer() || !pending) return false;
  finish();
  // A prior notice may have a different size or orientation. Replace its pixels.
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_LOADING));
  return true;
}

void BookLoadingFeedback::showIndexing(GfxRenderer& renderer) {
  finish();
  if (!renderer.hasFrameBuffer()) return;
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_INDEXING));
}
