#pragma once

#include <GfxRenderer.h>

namespace ReaderUtils {

// One scope per page presentation, under RenderLock. Keep it alive through AA
// and cleanup; every return restores menu mode. Buffer-only composition opts out.
// The SDK remembers the last displayed page for the first menu's erase waveform.
// Image pages disable only reader-specific combined text AA, preserving four tones.
class ReaderRefreshScope {
 public:
  explicit ReaderRefreshScope(const GfxRenderer& renderer, bool updatePanel = true, bool textOnly = true)
      : renderer(renderer), updatePanel(updatePanel) {
    if (updatePanel) renderer.setReaderRefresh(true, textOnly);
  }
  ~ReaderRefreshScope() {
    if (updatePanel) renderer.setReaderRefresh(false);
  }
  ReaderRefreshScope(const ReaderRefreshScope&) = delete;
  ReaderRefreshScope& operator=(const ReaderRefreshScope&) = delete;

 private:
  const GfxRenderer& renderer;
  const bool updatePanel;
};

}  // namespace ReaderUtils
