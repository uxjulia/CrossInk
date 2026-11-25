#pragma once

#include <FreeInkUIGfxRenderer.h>

#include "fontIds.h"

namespace ReaderSliderHints {
// Step hints use the same small built-in font at every UI scale.
inline void bindFont(freeink::ui::GfxRendererTarget& target) {
  target.setFont(freeink::ui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
}

inline freeink::ui::TextStyle style(const freeink::ui::ThemeTokens& theme) {
  auto hint = theme.smallText;
  hint.font = freeink::ui::GfxRendererTarget::FONT_LABEL;
  hint.align = freeink::ui::TextAlign::Center;
  return hint;
}
}  // namespace ReaderSliderHints
