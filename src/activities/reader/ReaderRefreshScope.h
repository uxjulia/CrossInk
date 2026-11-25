#pragma once

#include <GfxRenderer.h>

namespace ReaderUtils {

// One scope per page presentation, under RenderLock. Keep it alive through AA
// and cleanup; every return restores menu mode. Buffer-only composition opts out.
// The SDK remembers the last displayed page for the first menu's erase waveform.
// Image pages disable only reader-specific combined text AA, preserving four tones.
class ReaderRefreshScope {
 public:
  using Content = GfxRenderer::RefreshContent;
  static Content text(bool twoTone) { return twoTone ? Content::TwoToneText : Content::Text; }

  explicit ReaderRefreshScope(const GfxRenderer& renderer, Content content = Content::Text, bool updatePanel = true)
      : renderer(renderer), updatePanel(updatePanel) {
    if (updatePanel) renderer.setReaderRefresh(content);
  }
  ~ReaderRefreshScope() {
    if (updatePanel) renderer.setReaderRefresh(Content::Ui);
  }
  ReaderRefreshScope(const ReaderRefreshScope&) = delete;
  ReaderRefreshScope& operator=(const ReaderRefreshScope&) = delete;

 private:
  const GfxRenderer& renderer;
  const bool updatePanel;
};

}  // namespace ReaderUtils
