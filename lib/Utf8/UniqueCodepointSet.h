#pragma once

#include <algorithm>
#include <cstdint>

// Collects unique codepoints into a caller-owned buffer.
//
// The previous collectors compared every character against every codepoint seen
// so far. That is ~50 compares per Latin character and ~1,000+ per CJK
// character once a chapter has a few thousand unique glyphs. Here ASCII lives
// in a 128-bit bitmap (most Latin text never touches the buffer), and other
// codepoints are checked with a binary search over a sorted prefix. New ones
// append to an unsorted tail that is sorted/de-duplicated only when the buffer
// fills. No heap allocation; the object is a few dozen bytes of bookkeeping.
//
// At most `capacity` unique codepoints are kept. finish() returns them sorted.
class UniqueCodepointSet {
 public:
  UniqueCodepointSet(uint32_t* buffer, const uint32_t capacity) : buf_(buffer), cap_(capacity) {}

  // Returns true when `cp` is new but the capacity is already used, matching the
  // "cap hit" result of the linear-scan collectors. Zero is ignored.
  bool add(const uint32_t cp) {
    if (cp == 0) return false;
    if (cp < 128) {
      const uint32_t bit = 1u << (cp & 31u);
      uint32_t& word = ascii_[cp >> 5];
      if (word & bit) return false;
      if (!hasRoom()) return true;
      word |= bit;
      ++asciiCount_;
      return false;
    }
    if (std::binary_search(buf_, buf_ + sorted_, cp)) return false;
    if (!hasRoom()) {
      // The tail may already contain `cp`; compaction tells us for certain.
      if (std::binary_search(buf_, buf_ + sorted_, cp)) return false;
      if (!hasRoom()) return true;
    }
    buf_[count_++] = cp;
    return false;
  }

  // Sorts and de-duplicates the buffer, merges the ASCII bitmap into it and
  // returns the number of unique codepoints now in buffer[0..n). Call once,
  // after the last add().
  uint32_t finish() {
    compact();
    for (uint32_t cp = 1; cp < 128; ++cp) {
      if (ascii_[cp >> 5] & (1u << (cp & 31u))) buf_[count_++] = cp;
    }
    // Buffered codepoints are all >= 128, so the ASCII block moves to the front.
    std::rotate(buf_, buf_ + sorted_, buf_ + count_);
    return count_;
  }

 private:
  // True when one more unique codepoint fits, compacting the tail if needed.
  bool hasRoom() {
    if (count_ + asciiCount_ < cap_) return true;
    compact();
    return count_ + asciiCount_ < cap_;
  }

  void compact() {
    if (sorted_ == count_) return;
    std::sort(buf_, buf_ + count_);
    count_ = static_cast<uint32_t>(std::unique(buf_, buf_ + count_) - buf_);
    sorted_ = count_;
  }

  uint32_t* buf_;
  uint32_t cap_;
  uint32_t count_ = 0;   // entries in buf_ (sorted prefix + unsorted tail)
  uint32_t sorted_ = 0;  // length of the sorted, unique prefix
  uint32_t asciiCount_ = 0;
  uint32_t ascii_[4] = {};
};
