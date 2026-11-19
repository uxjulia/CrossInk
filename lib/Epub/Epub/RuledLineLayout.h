#pragma once
#include <algorithm>

// All coordinates are relative to the text origin. The ink bottom is exclusive.
struct RuledLineLayout {
  static constexpr int clearance = 2;
  int height;
  int offset;

  struct Ink {
    int top = 0, bottom = 0;
    enum class LetterCase : unsigned char { NoLetters, Capitals, Prose };
    LetterCase letters = LetterCase::NoLetters;
    void observeText(const char* text);
    void include(int first, int end) {
      if (end <= first) return;
      if (bottom <= top) {
        top = first;
        bottom = end;
      } else {
        top = std::min(top, first);
        bottom = std::max(bottom, end);
      }
    }
  };
  // The rule occupies the last pixel. Center the prose body in the remaining
  // space, reserving enough room on both sides for the font and the row's ink.
  template <typename Renderer>
  static RuledLineLayout center(const Renderer& renderer, int font, int requested, Ink ink, int fontTop = 0) {
    if (ink.bottom <= ink.top) return {requested, 0};
    int centerTwice = ink.top + ink.bottom;
    int span = ink.bottom - ink.top;
    typename Renderer::TextVerticalBounds body{}, extent{};
    if (renderer.getLatinBodyBounds(font, body, extent)) {
      const int bodyCenter = body.top + body.bottom;
      span = std::max({span, bodyCenter - 2 * extent.top, 2 * extent.bottom - bodyCenter});
      if (ink.letters != Ink::LetterCase::Capitals) {
        centerTwice = bodyCenter + 2 * fontTop;
        span = std::max({span, centerTwice - 2 * ink.top, 2 * ink.bottom - centerTwice});
      }
    }
    const int height = std::max(requested, span + 2 * clearance + 1);
    return {height, (height - 1 - centerTwice) / 2};
  }
};
