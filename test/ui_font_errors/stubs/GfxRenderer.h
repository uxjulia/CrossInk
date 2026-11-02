#pragma once
#include <EpdFont.h>

class GfxRenderer {
 public:
  unsigned bindings = 0;
  void clearFilenameFallbacks() { bindings = 0; }
  bool setFilenameFallback(int, int, const EpdFont* regular, const EpdFont*) {
    if (!regular) return false;
    ++bindings;
    return true;
  }
};
