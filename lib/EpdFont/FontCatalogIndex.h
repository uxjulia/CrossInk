#pragma once

#include <cstddef>
#include <cstdint>

namespace fontcatalog {
// Rebuildable cache: separate from user font files and EPUB caches.
inline constexpr char Path[] = "/.crosspoint/font-catalog.bin";
inline constexpr char TempPath[] = "/.crosspoint/font-catalog.tmp";
inline constexpr uint32_t Magic = 0x46434931;
inline constexpr uint32_t Version = 2;
#if CROSSINK_SCALABLE_FONTS
inline constexpr uint32_t Mode = 1;
#else
inline constexpr uint32_t Mode = 0;
#endif
inline constexpr uint32_t MaxBytes = 2 * 1024 * 1024;
inline constexpr uint16_t MaxFiles = 256;
inline constexpr uint16_t MaxPath = 255;
struct Header {
  uint32_t magic = Magic;
  uint32_t version = Version;
  uint64_t inventory = 0;
  uint32_t count = 0;
  uint32_t scalable = 0;
};
struct Entry {
  char name[128] = {};
  uint32_t offset = 0;
  uint32_t bytes = 0;
  uint32_t hash = 0;
  uint16_t count = 0;
  uint8_t first = 0;
  uint8_t last = 0;
  uint8_t scalable = 0;
  uint8_t reserved[3] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(Header) == 24 && sizeof(Entry) == 152, "Update font index format version");
inline uint32_t hashBytes(uint32_t hash, const void* bytes, size_t size) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 16777619u;
  return hash;
}
bool inventory(uint64_t& fingerprint);
}  // namespace fontcatalog
