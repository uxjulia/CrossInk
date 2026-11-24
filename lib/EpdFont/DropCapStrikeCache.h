#pragma once

#include <Memory.h>

#include <cstdint>

// One page composition owns this slot; subsequent grayscale passes borrow the
// same glyph without opening the SD file again. No allocation without a hit.
class DropCapStrikeCache {
 public:
  struct Bitmap {
    const uint8_t* pixels = nullptr;
    uint8_t width = 0;
    uint8_t height = 0;
  };

  Bitmap locate(int fontId, const char* bodyPath, uint8_t points, uint32_t cp, uint8_t style, uint16_t scale256,
                uint8_t bodyWidth, uint8_t bodyHeight);

 private:
  int sourceFont_ = 0;
  uint32_t sourceCodepoint_ = 0;
  uint8_t sourceStyle_ = 0;
  uint8_t sourceTarget_ = 0;
  int selectionFont_ = 0;
  uint8_t selectionTarget_ = 0;
  char selectionPath_[192]{};
  void select(int fontId, const char* bodyPath, uint8_t points, uint8_t target);
  size_t capacity_ = 0;
  HeapByteBuffer scratch_;
  Bitmap bitmap_{};

  bool load(const char* path, uint32_t cp, uint8_t style, uint8_t bodyWidth, uint8_t bodyHeight);
};
