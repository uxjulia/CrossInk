#pragma once

#include <cstdint>

struct JPEGFILE {
  int32_t iPos = 0;
  int32_t iSize = 0;
  void* fHandle = nullptr;
};

struct JPEGDRAW {
  int x = 0;
  int y = 0;
  int iWidth = 0;
  int iHeight = 0;
  int iWidthUsed = 0;
  int iBpp = 0;
  uint16_t* pPixels = nullptr;
  void* pUser = nullptr;
};

using JPEG_READ_CALLBACK = int32_t(JPEGFILE*, uint8_t*, int32_t);
using JPEG_SEEK_CALLBACK = int32_t(JPEGFILE*, int32_t);
using JPEG_DRAW_CALLBACK = int(JPEGDRAW*);
using JPEG_OPEN_CALLBACK = void*(const char*, int32_t*);
using JPEG_CLOSE_CALLBACK = void(void*);

inline constexpr int JPEG_MODE_PROGRESSIVE = 1;
inline constexpr int EIGHT_BIT_GRAYSCALE = 3;
inline constexpr int JPEG_SCALE_HALF = 2;
inline constexpr int JPEG_SCALE_QUARTER = 4;
inline constexpr int JPEG_SCALE_EIGHTH = 8;

namespace jpegdec_test {
inline int openResult = 1;
inline int decodeResult = 1;
inline int closeCalls = 0;
inline int width = 320;
inline int height = 240;

inline void reset() {
  openResult = 1;
  decodeResult = 1;
  closeCalls = 0;
  width = 320;
  height = 240;
}
}  // namespace jpegdec_test

class JPEGDEC {
 public:
  int open(const char* filename, JPEG_OPEN_CALLBACK* openCallback, JPEG_CLOSE_CALLBACK* closeCallback,
           JPEG_READ_CALLBACK*, JPEG_SEEK_CALLBACK*, JPEG_DRAW_CALLBACK*) {
    closeCallback_ = closeCallback;
    handle_ = (*openCallback)(filename, &file_.iSize);
    file_.fHandle = handle_;
    if (!handle_) return 0;
    return jpegdec_test::openResult;
  }

  int openRAM(uint8_t*, int, JPEG_DRAW_CALLBACK*) { return jpegdec_test::openResult; }

  void close() {
    ++jpegdec_test::closeCalls;
    if (closeCallback_) (*closeCallback_)(handle_);
  }

  int getLastError() const { return 77; }
  int getWidth() const { return jpegdec_test::width; }
  int getHeight() const { return jpegdec_test::height; }
  int getJPEGType() const { return 0; }
  void setPixelType(int) {}
  void setUserPointer(void*) {}
  int decode(int, int, int) { return jpegdec_test::decodeResult; }

 private:
  JPEGFILE file_{};
  void* handle_ = nullptr;
  JPEG_CLOSE_CALLBACK* closeCallback_ = nullptr;
  uint8_t padding_[128]{};
};
