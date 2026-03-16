#pragma once
#include <cstdint>
#ifndef EPD_SCLK
#define EPD_SCLK 0
#endif
#ifndef EPD_MOSI
#define EPD_MOSI 0
#endif
#ifndef EPD_CS
#define EPD_CS 0
#endif
#ifndef EPD_DC
#define EPD_DC 0
#endif
#ifndef EPD_RST
#define EPD_RST 0
#endif
#ifndef EPD_BUSY
#define EPD_BUSY 0
#endif

class EInkDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;

  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  EInkDisplay() = default;
  EInkDisplay(int, int, int, int, int, int) {}
  void begin() {}
  void clearScreen(uint8_t color) {}
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) {}
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) {}
  void displayBuffer(RefreshMode mode, bool turnOffScreen) {}
  void refreshDisplay(RefreshMode mode, bool turnOffScreen) {}
  void deepSleep() {}
  uint8_t* getFrameBuffer() {
    static uint8_t buf[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];
    return buf;
  }
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {}
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {}
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {}
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {}
  void displayGrayBuffer(bool turnOffScreen = false) {}
};
