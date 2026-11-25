#pragma once

#include <array>
#include <cstdint>

// Physical edges in portrait orientation. Rotation belongs to the renderer;
// this value never changes touch coordinates or the framebuffer dimensions.
struct ScreenInsets {
  static constexpr uint8_t MAX_INSET = 32;
  static constexpr uint8_t MAX_CORNER = 96;
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  std::array<uint8_t, 4> edges{{0, 0, 0, 0}};  // Top, right, bottom, left.
#else
  std::array<uint8_t, 4> edges{{9, 3, 3, 3}};
#endif

  // Horizontal clearance at physical portrait top/bottom corners.
#if !defined(SIMULATOR) && defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4
  std::array<uint8_t, 2> cornerInsets{{35, 35}};
#else
  std::array<uint8_t, 2> cornerInsets{{0, 0}};
#endif

  std::array<int, 4> corners(unsigned orientation) const {
    const std::array<int, 4> portrait{{cornerInsets[0], cornerInsets[0], cornerInsets[1], cornerInsets[1]}};
    std::array<int, 4> result{};
    for (unsigned i = 0; i < 4; ++i) result[i] = portrait[(i + 4 - orientation % 4) % 4];
    return result;
  }

  void adjustCorner(unsigned corner, int delta) {
    if (corner >= cornerInsets.size()) return;
    const int next = static_cast<int>(cornerInsets[corner]) + delta;
    cornerInsets[corner] = static_cast<uint8_t>(next < 0 ? 0 : (next > MAX_CORNER ? MAX_CORNER : next));
  }

  constexpr bool valid() const {
    return edges[0] <= MAX_INSET && edges[1] <= MAX_INSET && edges[2] <= MAX_INSET && edges[3] <= MAX_INSET &&
           cornerInsets[0] <= MAX_CORNER && cornerInsets[1] <= MAX_CORNER;
  }
  bool operator==(const ScreenInsets& other) const {
    return edges == other.edges && cornerInsets == other.cornerInsets;
  }
  bool operator!=(const ScreenInsets& other) const { return !(*this == other); }

  ScreenInsets rotated(unsigned orientation) const {
    ScreenInsets result = *this;
    for (unsigned edge = 0; edge < 4; ++edge) result.edges[edge] = edges[(edge + 4 - orientation % 4) % 4];
    return result;
  }

  void adjust(unsigned edge, int delta) {
    if (edge >= edges.size()) return;
    const int next = static_cast<int>(edges[edge]) + delta;
    edges[edge] = static_cast<uint8_t>(next < 0 ? 0 : (next > MAX_INSET ? MAX_INSET : next));
  }

  int topOrigin(unsigned orientation, int legacyOrigin) const {
    const int top = rotated(orientation).edges[0];
    const int adjusted = legacyOrigin + top - ScreenInsets{}.rotated(orientation).edges[0];
    return adjusted < top ? top : adjusted;
  }
};
