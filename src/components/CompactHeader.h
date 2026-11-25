#pragma once

class GfxRenderer;
struct ThemeMetrics;
struct Rect;

namespace CompactHeader {
int height(const ThemeMetrics& metrics);
int headerBottomY(const ThemeMetrics& metrics);
int contentTop(const ThemeMetrics& metrics);
Rect headerRect(const GfxRenderer& renderer);
int headerBottomY(const GfxRenderer& renderer);
int contentTop(const GfxRenderer& renderer);
void drawTitle(const GfxRenderer& renderer, const char* title, bool showDate = false,
               const Rect* headerOverride = nullptr);
}  // namespace CompactHeader
