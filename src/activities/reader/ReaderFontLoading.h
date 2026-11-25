#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace ReaderUtils {
// A scalable catalog summary does not guarantee that its font bytes are still
// loaded: network handoff, low-memory recovery, or a replaced file may require
// another SD read. Only an unchanged resident family can preview another point
// size without blocking.
inline bool shouldShowFontPreviewLoading(const char* familyName) {
  return !sdFontSystem.canResizeResidentScalableFamilyWithoutReload(familyName);
}

// Gesture/button callers run on the main loop. Publish feedback before cold
// metadata reads, then leave the caller's existing reflow/indexing flow in charge.
inline bool changeReaderFontSizeWithFeedback(GfxRenderer& renderer, const bool larger,
                                             const FontSizeStepMode mode = FontSizeStepMode::Wrap) {
  RenderLock lock;
  const auto& registry = sdFontSystem.registry();
  bool showLoading = false;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* summary = registry.findSummary(SETTINGS.sdFontFamilyName);
    showLoading = registry.needsRefresh() || !summary || summary->files.empty();
  }
  if (showLoading) GUI.drawPopup(renderer, tr(STR_LOADING), true);
  const bool changed = sdFontSystem.changeReaderFontSize(larger, mode);
  // A clamped step or discovery failure has no subsequent reflow to clear the
  // physical popup. Its backing pixels have already been restored.
  if (showLoading && !changed) renderer.displayBuffer();
  return changed;
}
}  // namespace ReaderUtils
