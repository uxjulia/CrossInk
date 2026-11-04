#pragma once

#include "TouchActionButtons.h"

class GfxRenderer;
struct ThemeMetrics;

// Shared comparison cards for server and Nearby progress sync.
namespace ProgressSyncComparison {

struct Card {
  const char* title;
  const char* source;  // Optional, e.g. the remote device name.
  const char* pageText;
  const char* chapter;
  float percentage;  // 0.0 - 1.0
  bool ahead;
};

long displayedTenths(float percentage);
TouchActionButtons::Layout actionLayout(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics,
                                        bool hasTouch, uint8_t buttonCount = 2);
void drawCards(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, int top,
               const Card& remote, const Card& local);

}  // namespace ProgressSyncComparison
