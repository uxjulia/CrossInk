#include <cassert>

#include "BookLoadingFeedback.h"
#include "components/UITheme.h"

TestTheme GUI;

int main() {
  GfxRenderer renderer;
  BookLoadingFeedback feedback;
  assert(!feedback.showIfPending(renderer));  // Direct reader construction is quiet.

  feedback.start();
  renderer.available = false;
  assert(!feedback.showIfPending(renderer));  // Never touch lent build scratch.
  renderer.available = true;
  assert(feedback.showIfPending(renderer));
  assert(GUI.draws == 1);
  assert(renderer.clears == 1);
  assert(!feedback.showIfPending(renderer));  // One refresh per open.

  feedback.start();
  feedback.finish();  // Failure or an existing Indexing popup.
  assert(!feedback.showIfPending(renderer));

  feedback.start();
  BookLoadingFeedback handedOff = feedback;
  assert(handedOff.showIfPending(renderer));  // Pending feedback survives the reader handoff.
  assert(GUI.draws == 2);
  BookLoadingFeedback afterPopup = handedOff;
  assert(!afterPopup.showIfPending(renderer));

  // Indexing replaces an already visible Loading notice and cancels future Loading.
  feedback.start();
  feedback.showIndexing(renderer);
  assert(GUI.draws == 3);
  assert(renderer.clears == 3);
  assert(!feedback.showIfPending(renderer));
  feedback.showIndexing(renderer);
  assert(GUI.draws == 4);
  assert(renderer.clears == 4);

  // Never clear a framebuffer lent to EPUB parsing, even when indexing starts.
  feedback.start();
  renderer.available = false;
  feedback.showIndexing(renderer);
  assert(renderer.clears == 4);
  renderer.available = true;
  assert(!feedback.showIfPending(renderer));
}
