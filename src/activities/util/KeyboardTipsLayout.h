#pragma once

// Adapted from CrossPoint PR #3863 (Uri Tauber). Keep the reserved rows
// independent of text emptiness so typing the first character cannot move tips.
constexpr int keyboardTipsY(const int fieldBottom, const int keyboardTop, const int lineHeight, const bool cursorMode,
                            const bool password, const bool urlPanel, const bool symbols, const bool urlInput) {
  const int tipRows = cursorMode ? 1 : urlPanel ? 3 : symbols ? 2 : 3 + (urlInput ? 1 : 0);
  const int height = (tipRows + 1) * lineHeight;  // Include the heading.
  const int top = fieldBottom + (cursorMode ? (password ? 2 : 1) * lineHeight : 0);
  return lineHeight > 0 && keyboardTop - top >= height ? top + (keyboardTop - top - height) / 2 : -1;
}
