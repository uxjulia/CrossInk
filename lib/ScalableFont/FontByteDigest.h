#pragma once

#include <cstddef>
#include <cstdint>

// Keep the existing byte-for-byte FNV identity and sfnt checksum. The bulk
// loop handles a whole checksum word at a time instead of updating checksum
// bookkeeping and branching for every byte of a multi-megabyte SD font.
class FontByteDigest {
 public:
  explicit FontByteDigest(uint32_t revision) : hash_(2166136261u ^ revision) {}

  void add(const uint8_t* bytes, size_t size) {
    while (size && (totalBytes_ < 4 || wordBytes_)) {
      addByte(*bytes++);
      --size;
    }
    const size_t wholeBytes = size & ~size_t(3);
    totalBytes_ += wholeBytes;
    for (size_t i = 0; i < wholeBytes; i += 4) {
      const uint32_t a = bytes[i], b = bytes[i + 1], c = bytes[i + 2], d = bytes[i + 3];
      hash_ = (hash_ ^ a) * 16777619u;
      hash_ = (hash_ ^ b) * 16777619u;
      hash_ = (hash_ ^ c) * 16777619u;
      hash_ = (hash_ ^ d) * 16777619u;
      sum_ += (a << 24) | (b << 16) | (c << 8) | d;
    }
    bytes += wholeBytes;
    size -= wholeBytes;
    while (size--) addByte(*bytes++);
  }

  uint32_t hash() const { return hash_; }
  uint32_t checksum() const { return wordBytes_ ? sum_ + (word_ << (8 * (4 - wordBytes_))) : sum_; }
  bool integrityMismatch() const {
    const bool singleFace = signature_ == 0x00010000u || signature_ == 0x4F54544Fu || signature_ == 0x74727565u ||
                            signature_ == 0x74797031u;
    return totalBytes_ >= 12 && singleFace && checksum() != 0xB1B0AFBAu;
  }

 private:
  void addByte(uint8_t byte) {
    hash_ = (hash_ ^ byte) * 16777619u;
    word_ = (word_ << 8) | byte;
    ++wordBytes_;
    ++totalBytes_;
    if (wordBytes_ == 4) {
      if (totalBytes_ == 4) signature_ = word_;
      sum_ += word_;
      word_ = 0;
      wordBytes_ = 0;
    }
  }

  uint32_t hash_;
  uint32_t sum_ = 0;
  uint32_t word_ = 0;
  uint32_t signature_ = 0;
  size_t totalBytes_ = 0;
  uint8_t wordBytes_ = 0;
};
