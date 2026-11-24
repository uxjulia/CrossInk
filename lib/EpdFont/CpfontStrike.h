#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Narrow random-access decoder for one 2bpp glyph from a larger CPFont v4
// strike. Never instantiates a face or reads its kerning, shaping, or other glyphs.
namespace cpfontStrike {

constexpr uint16_t FormatVersion = 4;
constexpr size_t MaxGlyphBytes = 4096;
constexpr uint8_t MaxContentPoints = 44;

inline constexpr uint8_t targetPoints(uint8_t body, uint16_t scale256) {
  return static_cast<uint8_t>(std::clamp<unsigned>((unsigned(body) * scale256 + 128) / 256, 8, MaxContentPoints));
}

inline constexpr bool preferPoints(uint8_t body, uint8_t target, uint8_t current, uint8_t candidate) {
  if (candidate <= body || candidate > MaxContentPoints) return false;
  const auto distance = [target](int points) { return points >= target ? points - target : target - points; };
  return !current || distance(candidate) < distance(current) ||
         (distance(candidate) == distance(current) && candidate > current);
}

struct Reader {
  void* context;
  uint32_t size;
  bool (*read)(void*, uint32_t offset, void* out, size_t bytes);

  bool at(uint32_t offset, void* out, size_t bytes) const {
    return offset <= size && bytes <= size - offset && read(context, offset, out, bytes);
  }
};

struct Glyph {
  uint32_t offset = 0;
  uint16_t bytes = 0;
  uint8_t width = 0;
  uint8_t height = 0;
};

inline uint32_t u16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t* p) { return u16(p) | u16(p + 2) << 16; }

inline bool find(const Reader& file, uint8_t style, uint32_t cp, Glyph& out) {
  constexpr unsigned HeaderBytes = 32, TocBytes = 32, IntervalBytes = 12, GlyphBytes = 16;
  uint8_t data[TocBytes];
  if (style >= 4 || !file.at(0, data, HeaderBytes) || std::memcmp(data, "CPFONT\0\0", 8) != 0 ||
      u16(data + 8) != FormatVersion || !(u16(data + 10) & 1))
    return false;
  const uint8_t styles = data[12];
  if (!styles || styles > 4) return false;
  const uint32_t dataStart = HeaderBytes + styles * TocBytes;

  uint32_t starts[4]{}, shaping[4]{};
  uint32_t intervals = 0, glyphs = 0, start = 0;
  uint64_t glyphTable = 0, bitmap = 0;
  for (uint8_t i = 0; i < styles; ++i) {
    if (!file.at(HeaderBytes + i * TocBytes, data, TocBytes)) return false;
    const uint8_t id = data[0];
    if (id >= 4 || starts[id]) return false;
    starts[id] = u32(data + 24);
    shaping[id] = u32(data + 28);
    if (starts[id] < dataStart || starts[id] >= file.size ||
        (shaping[id] && (shaping[id] <= starts[id] || shaping[id] >= file.size)))
      return false;
    if (id != style) continue;

    start = starts[id];
    intervals = u32(data + 4);
    glyphs = u32(data + 8);
    const uint32_t kernLeft = u16(data + 17), kernRight = u16(data + 19);
    if (!intervals || intervals > 4096 || !glyphs || glyphs > 65536 || kernLeft > 4096 || kernRight > 4096)
      return false;
    glyphTable = uint64_t(start) + intervals * IntervalBytes;
    bitmap = glyphTable + glyphs * GlyphBytes + (kernLeft + kernRight) * 3 + uint32_t(data[21]) * data[22] +
             uint32_t(data[23]) * 8;
  }
  if (!start) return false;
  uint32_t end = file.size;
  for (unsigned i = 0; i < 4; ++i) {
    if (i != style && starts[i] == start) return false;
    if (starts[i] > start) end = std::min(end, starts[i]);
    if (shaping[i] > start) end = std::min(end, shaping[i]);
  }
  if (bitmap >= end) return false;

  // The format's intervals are sorted and glyph indices are contiguous.
  // Read at most 13 records, even when the glyph is absent.
  uint32_t first = 0, last = intervals;
  while (first < last) {
    const uint32_t middle = first + (last - first) / 2;
    if (!file.at(start + middle * IntervalBytes, data, IntervalBytes)) return false;
    const uint32_t min = u32(data), max = u32(data + 4), index = u32(data + 8);
    if (min > max || max > 0x10ffff || uint64_t(index) + max - min >= glyphs) return false;
    if (cp < min)
      last = middle;
    else if (cp > max)
      first = middle + 1;
    else {
      if (!file.at(static_cast<uint32_t>(glyphTable + (index + cp - min) * GlyphBytes), data, GlyphBytes)) return false;
      const uint8_t width = data[0], height = data[1];
      const uint32_t bytes = u16(data + 8), displacement = u32(data + 12);
      const uint64_t offset = bitmap + displacement;
      if (!width || !height || bytes > MaxGlyphBytes || bytes != (unsigned(width) * height + 3) / 4 ||
          offset + bytes > end)
        return false;
      out = {static_cast<uint32_t>(offset), static_cast<uint16_t>(bytes), width, height};
      return true;
    }
  }
  return false;
}

}  // namespace cpfontStrike
