#pragma once
#include <initializer_list>

#include "UiAppHost.h"

class GfxRenderer;

// Themed FreeInkUI screen builders for SD-plugin catalogs.

// Draw the unified themed header (status band, title, rule, and a back button
// on touch boards) and reserve the button-hint band below the content. A
// trailing icon (e.g. search) becomes a header button.
void catalogScreenHeader(UiAppHost<24, 6>::Screen& screen, GfxRenderer& renderer, const MappedInputManager& input,
                         freeink::ui::GfxRendererTarget& target, const char* title,
                         const freeink::ui::BitmapRef& trailingIcon = {},
                         freeink::ui::ActionId trailingAction = freeink::ui::NO_ACTION);

// One line of a centered message block; bold marks a heading line.
struct CatalogLine {
  const char* text;
  bool bold = false;
};

// Vertically centered stack of short lines in the remaining content band
// (error screens, completion notices, sign-in hints).
void catalogCenteredBlock(UiAppHost<24, 6>::Screen& screen, std::initializer_list<CatalogLine> lines);

// Centered download screen: heading, item title, download progress and, when
// cancelAction is a real action, a Cancel button. A progress bar is shown when
// the total is known (total > 0), otherwise a running byte count.
void catalogDownloadScreen(UiAppHost<24, 6>::Screen& screen, const char* status, size_t progress, size_t total,
                           freeink::ui::ActionId cancelAction = freeink::ui::NO_ACTION);
