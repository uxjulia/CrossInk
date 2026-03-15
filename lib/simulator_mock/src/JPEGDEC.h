#pragma once
// Simulator stub for JPEGDEC (bitbank2/JPEGDEC)
// The real library is only available on Arduino/ESP32 targets.
// All decode operations return failure so callers skip image rendering gracefully.

#include <cstdint>
#include <cstddef>

// Scale options passed to decode()
#define JPEG_SCALE_EIGHTH  4
#define JPEG_SCALE_QUARTER 2
#define JPEG_SCALE_HALF    1

// Pixel output types
#define EIGHT_BIT_GRAYSCALE 1
#define RGB565_LITTLE_ENDIAN 2

// JPEG stream types
#define JPEG_MODE_BASELINE    0
#define JPEG_MODE_PROGRESSIVE 1

struct JPEGFILE {
    void*   fHandle;
    int32_t iPos;
    int32_t iSize;
};

struct JPEGDRAW {
    void*    pUser;
    uint8_t* pPixels;
    int      x;
    int      y;
    int      iWidth;
    int      iHeight;
    int      iWidthUsed;
};

using JPEG_DRAW_CALLBACK  = int (*)(JPEGDRAW*);
using JPEG_OPEN_CALLBACK  = void* (*)(const char*, int32_t*);
using JPEG_CLOSE_CALLBACK = void (*)(void*);
using JPEG_READ_CALLBACK  = int32_t (*)(JPEGFILE*, uint8_t*, int32_t);
using JPEG_SEEK_CALLBACK  = int32_t (*)(JPEGFILE*, int32_t);

class JPEGDEC {
public:
    int open(const char*, JPEG_OPEN_CALLBACK, JPEG_CLOSE_CALLBACK,
             JPEG_READ_CALLBACK, JPEG_SEEK_CALLBACK, JPEG_DRAW_CALLBACK) { return 0; }
    void close() {}
    int  getWidth()     const { return 0; }
    int  getHeight()    const { return 0; }
    int  getLastError() const { return -1; }
    int  getJPEGType()  const { return JPEG_MODE_BASELINE; }
    void setPixelType(int)    {}
    void setUserPointer(void*) {}
    int  decode(int, int, int) { return 0; }
};
