#pragma once
#include <GfxRenderer.h>

#include <cassert>
struct TestTheme {
  int draws = 0;
  void drawPopup(GfxRenderer& renderer, const char*) {
    assert(!renderer.hasOldNotice);
    renderer.hasOldNotice = true;
    ++draws;
  }
};
extern TestTheme GUI;
