#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <esp_rom_crc.h>
#endif

namespace OptimizerFormat {
constexpr uint32_t MAX_RAW = 128 * 1024;
constexpr uint16_t BLOCK_SIZE = 2048;
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
inline void put16(uint8_t* p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
inline void put32(uint8_t* p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}
inline uint32_t crc(uint32_t value, const uint8_t* p, size_t n) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return esp_rom_crc32_le(value, p, n);
#else
  value = ~value;
  while (n--) {
    value ^= *p++;
    for (int i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1)));
  }
  return ~value;
#endif
}
inline bool dimensions(uint16_t w, uint16_t h) {
  return w && h && w <= 1024 && h <= 1024 && uint32_t((w + 3) / 4) * h <= MAX_RAW;
}
struct Record {
  char href[129] = {};
  char pxcHref[65] = {};
  uint16_t width = 0, height = 0;
  uint8_t format = 1, flags = 0;
  uint32_t bytes = 0, pixelCrc = 0;
};
static_assert(sizeof(Record) <= 256, "Index records must remain small");
inline bool path(const char* s, size_t capacity) {
  const auto* end = static_cast<const char*>(memchr(s, 0, capacity));
  if (!end || end == s || s[0] == '/') return false;
  for (auto p = s; p != end; ++p) {
    if (static_cast<unsigned char>(*p) < 32 || *p == '\\' || *p == ':' || *p == '%' ||
        (*p == '.' && p + 1 != end && p[1] == '.'))
      return false;
  }
  return true;
}
inline bool valid(const Record& r) {
  if (!path(r.href, sizeof(r.href)) || !path(r.pxcHref, sizeof(r.pxcHref)) ||
      strncmp(r.pxcHref, "META-INF/crossink/pxc/", 22) || !dimensions(r.width, r.height) || r.flags)
    return false;
  const uint32_t raw = uint32_t((r.width + 3) / 4) * r.height;
  return (r.format == 1 && r.bytes == raw + 4) ||
         (r.format == 2 && r.bytes >= 45 && r.bytes <= 32 + raw + 12 * ((raw + 2047) / 2048));
}
inline void encodeRecord(const Record& r, uint8_t* b) {
  memset(b, 0, 208);
  memcpy(b, r.href, 129);
  memcpy(b + 129, r.pxcHref, 65);
  put16(b + 194, r.width);
  put16(b + 196, r.height);
  b[198] = r.format;
  b[199] = r.flags;
  put32(b + 200, r.bytes);
  put32(b + 204, r.pixelCrc);
}
inline bool decodeRecord(const uint8_t* b, Record& r) {
  memcpy(r.href, b, 129);
  memcpy(r.pxcHref, b + 129, 65);
  r.width = u16(b + 194);
  r.height = u16(b + 196);
  r.format = b[198];
  r.flags = b[199];
  r.bytes = u32(b + 200);
  r.pixelCrc = u32(b + 204);
  return valid(r);
}
// COIX offsets: magic, version:u16, header:u16, record:u16, count:u16,
// flags:u32, manifest CRC:u32, manifest size:u32, records CRC:u32, header CRC:u32.
inline void indexHeader(uint8_t* b, uint16_t count, uint32_t manifestCrc, uint32_t manifestSize, uint32_t recordsCrc) {
  memset(b, 0, 32);
  memcpy(b, "COIX", 4);
  put16(b + 4, 1);
  put16(b + 6, 32);
  put16(b + 8, 208);
  put16(b + 10, count);
  put32(b + 16, manifestCrc);
  put32(b + 20, manifestSize);
  put32(b + 24, recordsCrc);
  put32(b + 28, crc(0, b, 28));
}
inline bool validHeader(const uint8_t* b, uint32_t manifestCrc, uint32_t manifestSize, size_t size) {
  return !memcmp(b, "COIX", 4) && u16(b + 4) == 1 && u16(b + 6) == 32 && u16(b + 8) == 208 && u16(b + 10) <= 256 &&
         !u32(b + 12) && u32(b + 16) == manifestCrc && u32(b + 20) == manifestSize && u32(b + 28) == crc(0, b, 28) &&
         size == 32U + 208U * u16(b + 10);
}
}  // namespace OptimizerFormat
