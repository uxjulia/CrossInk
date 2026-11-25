#pragma once

class GfxRenderer;
struct Rect;

// The block below a reader book header: an optional chapter title line and the
// compact progress sub-header, closed by a rule. Shared by the button reader
// menu and the frontlight drawer so both stay the same height and style.
namespace ReaderBookSummary {
// Pass nullptr or "" to omit the chapter line.
int height(const GfxRenderer& renderer, const char* chapter);
// Draws at rect's top-left; rect.height is ignored in favor of height().
void draw(const GfxRenderer& renderer, const Rect& rect, const char* chapter, const char* progress);
}  // namespace ReaderBookSummary
