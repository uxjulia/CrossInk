#pragma once
// Simulator stub for PNGdec (bitbank2/PNGdec)
// The real library is only available on Arduino/ESP32 targets.
// All decode operations return failure so callers skip image rendering gracefully.

#include <cstddef>
#include <cstdint>

#define PNG_SUCCESS 0
#define PNG_INVALID_FILE -1

// Pixel types
#define PNG_PIXEL_GRAYSCALE 0
#define PNG_PIXEL_TRUECOLOR 2
#define PNG_PIXEL_INDEXED 3
#define PNG_PIXEL_GRAY_ALPHA 4
#define PNG_PIXEL_TRUECOLOR_ALPHA 6

struct PNGFILE {
  void* fHandle;
};

struct PNGDRAW {
  void* pUser;
  uint8_t* pPixels;
  uint8_t* pPalette;
  int y;
  int iPixelType;
  int iHasAlpha;
  int iWidth;
};

using PNG_DRAW_CALLBACK = int (*)(PNGDRAW*);
using PNG_OPEN_CALLBACK = void* (*)(const char*, int32_t*);
using PNG_CLOSE_CALLBACK = void (*)(void*);
using PNG_READ_CALLBACK = int32_t (*)(PNGFILE*, uint8_t*, int32_t);
using PNG_SEEK_CALLBACK = int32_t (*)(PNGFILE*, int32_t);

class PNG {
 public:
  int open(const char*, PNG_OPEN_CALLBACK, PNG_CLOSE_CALLBACK, PNG_READ_CALLBACK, PNG_SEEK_CALLBACK,
           PNG_DRAW_CALLBACK) {
    return PNG_INVALID_FILE;
  }
  void close() {}
  int getWidth() const { return 0; }
  int getHeight() const { return 0; }
  int getBpp() const { return 8; }
  int getPixelType() const { return PNG_PIXEL_GRAYSCALE; }
  int decode(void*, int) { return PNG_INVALID_FILE; }
};
