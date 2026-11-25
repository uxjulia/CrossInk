#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "../../lib/ScalableFont/FontByteDigest.h"

int main() {
  std::vector<uint8_t> bytes(1030);
  uint32_t random = 12345;
  for (auto& byte : bytes) {
    random = random * 1664525u + 1013904223u;
    byte = random >> 24;
  }
  // Include unaligned inputs, non-word lengths and chunks crossing words.
  for (size_t offset = 0; offset < 4; ++offset) {
    for (size_t size = 0; size <= 1025; ++size) {
      uint32_t hash = 2166136261u ^ 9u;
      uint32_t checksum = 0;
      for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[offset + i]) * 16777619u;
        checksum += uint32_t(bytes[offset + i]) << (24 - (i % 4) * 8);
      }
      for (size_t chunk : {size_t(1), size_t(3), size_t(4), size_t(17), size_t(256), size_t(4096)}) {
        FontByteDigest digest(9);
        for (size_t i = 0; i < size; i += chunk) digest.add(bytes.data() + offset + i, std::min(chunk, size - i));
        assert(digest.hash() == hash);
        assert(digest.checksum() == checksum);
        assert(!digest.integrityMismatch());
      }
    }
  }
  for (uint32_t signature : {0x00010000u, 0x4F54544Fu, 0x74727565u, 0x74797031u}) {
    uint8_t sfnt[12] = {};
    const uint32_t adjustment = 0xB1B0AFBAu - signature;
    for (int i = 0; i < 4; ++i) {
      sfnt[i] = signature >> (24 - i * 8);
      sfnt[8 + i] = adjustment >> (24 - i * 8);
    }
    FontByteDigest valid(9);
    valid.add(sfnt, sizeof(sfnt));
    assert(!valid.integrityMismatch());
    sfnt[7] ^= 1;
    FontByteDigest corrupt(9);
    corrupt.add(sfnt, sizeof(sfnt));
    assert(corrupt.integrityMismatch());
    FontByteDigest truncated(9);
    truncated.add(sfnt, 11);
    assert(!truncated.integrityMismatch());
  }
}
