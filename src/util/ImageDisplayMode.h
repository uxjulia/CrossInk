#pragma once

#include <cstdint>

// Present the existing image/layout and grayscale preferences as one UI choice.
// Keep their saved values unchanged so global settings and book overrides remain compatible.
namespace ImageDisplayMode {
enum Mode : uint8_t { Grayscale, BlackAndWhite, Placeholder, Hidden, Count };

constexpr uint8_t selected(const uint8_t rendering, const uint8_t grayscale) {
  return rendering == 1 ? Placeholder : rendering == 2 ? Hidden : grayscale ? Grayscale : BlackAndWhite;
}

inline void apply(const uint8_t mode, uint8_t& rendering, uint8_t& grayscale) {
  switch (mode) {
    case Grayscale:
    case BlackAndWhite:
      rendering = 0;
      grayscale = mode == Grayscale;
      break;
    case Placeholder:
      rendering = 1;
      break;
    case Hidden:
      rendering = 2;
      break;
  }
}
}  // namespace ImageDisplayMode
