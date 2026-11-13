#pragma once
#include <Epub/blocks/ImageBlock.h>
#include <GfxRenderer.h>

#include <vector>
class Page {
 public:
  struct PlacedImage {
    ImageBlock* image;
    int x, y;
  };
  std::vector<PlacedImage> images;
  mutable int allVisits = 0, imageVisits = 0;
  int failImageVisit = 0;
  bool renderImages(GfxRenderer& renderer, int, int x, int y) const {
    if (++imageVisits == failImageVisit) return false;
    for (auto& item : images)
      if (!item.image->render(renderer, x + item.x, y + item.y, true)) return false;
    return true;
  }
  void renderText(GfxRenderer&, int, int, int, bool) const { ++allVisits; }
  void render(GfxRenderer& renderer, int font, int x, int y, bool black) const {
    renderText(renderer, font, x, y, black);
    renderImages(renderer, font, x, y);
  }
};
