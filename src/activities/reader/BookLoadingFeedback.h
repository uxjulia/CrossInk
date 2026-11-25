#pragma once

class GfxRenderer;

// Passed by value through the loader/reader handoff. Check only between loading
// stages, while the framebuffer is available and before painting page contents.
class BookLoadingFeedback {
  bool pending = false;

 public:
  void start() { pending = true; }
  void finish() { pending = false; }
  bool showIfPending(GfxRenderer& renderer);
  void showIndexing(GfxRenderer& renderer);
};
