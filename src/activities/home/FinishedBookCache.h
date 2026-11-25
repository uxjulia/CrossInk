#pragma once

#include <cstdint>

// Remembers the finished state of recently drawn books, keyed by a path hash,
// so list screens read a book's stats from SD once instead of on every render
// or scroll step. 32 slots cover a full page at the smallest UI scale for
// 136 bytes; older entries are replaced first.
class FinishedBookCache {
 public:
  static constexpr uint8_t CAPACITY = 32;

  bool lookup(const uint32_t key, bool& finished) const {
    for (uint8_t i = 0; i < count; ++i) {
      if (keys[i] == key) {
        finished = (bits >> i) & 1u;
        return true;
      }
    }
    return false;
  }

  void store(const uint32_t key, const bool finished) {
    uint8_t slot = next;
    if (count < CAPACITY) {
      slot = count++;
    } else {
      next = static_cast<uint8_t>((next + 1) % CAPACITY);
    }
    keys[slot] = key;
    if (finished) {
      bits |= 1u << slot;
    } else {
      bits &= ~(1u << slot);
    }
  }

  void clear() {
    count = 0;
    next = 0;
    bits = 0;
  }

 private:
  uint32_t keys[CAPACITY]{};
  uint32_t bits = 0;
  uint8_t count = 0;
  uint8_t next = 0;
};
