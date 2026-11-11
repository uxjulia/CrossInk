#pragma once
#include <GrayscaleCapabilities.h>

#include <cstdint>
#include <cstring>
#include <vector>
class HalDisplay {
 public:
  static constexpr int DISPLAY_WIDTH = 792, DISPLAY_HEIGHT = 481, DISPLAY_WIDTH_BYTES = 99, BUFFER_SIZE = 99 * 481;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  bool absoluteSupported = true;
  bool overlaySupported = true;
  bool combinedTextSupported = false;
  bool invertedOverlaySupported = false;
  int canceled = 0;
  bool readerRefresh = false;
  std::vector<bool> refreshHints;
  void setReaderRefresh(bool reading) { readerRefresh = reading; }
  struct RetainedOverlay {
    uint16_t x, y, width, height;
    bool restore;
  };
  std::vector<RetainedOverlay> retainedOverlays;
  bool displayRetainedGrayscaleOverlay(uint16_t x, uint16_t y, uint16_t width, uint16_t height, bool restore) {
    retainedOverlays.push_back({x, y, width, height, restore});
    return true;
  }
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode) const {
    GrayscaleCapabilities caps;
    if (mode == GrayscaleMode::Absolute) {
      if (absoluteSupported) caps.encoding = freeink::GrayscaleEncoding::AbsolutePlanes;
    } else if (overlaySupported && (!inverted || invertedOverlaySupported)) {
      caps.encoding = freeink::GrayscaleEncoding::OverlayMasks;
      if (readerRefresh && combinedTextSupported) caps.base = GrayscaleBase::Combined;
    }
    return caps;
  }
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode, bool) { return absoluteSupported; }
  int width, height, stride;
  mutable std::vector<uint8_t> bw;
  HalDisplay(int w = 792, int h = 481) : width(w), height(h), stride((w + 7) / 8), bw(stride * h, 0xA5) {}
  uint8_t* getFrameBuffer() const { return bw.data(); }
  int getDisplayWidth() const { return width; }
  int getDisplayHeight() const { return height; }
  int getDisplayWidthBytes() const { return stride; }
  uint32_t getBufferSize() const { return bw.size(); }
  void clearScreen(uint8_t c) const { memset(bw.data(), c, bw.size()); }
  bool inverted = false;
  bool isInverted() const { return inverted; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    *size = bw.size();
    return bw.data();
  }
  void returnFrameBufferStorage() {}
  void drawImage(const uint8_t*, int, int, int, int) {}
  void displayBuffer(RefreshMode, bool) { refreshHints.push_back(readerRefresh); }
  void displayBufferAsync(RefreshMode) {}
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return true; }
  bool supportsDeferredGrayscaleBase() const { return false; }
  bool displayGrayscaleBaseAsync(RefreshMode) { return false; }
  void copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
  bool supportsAsyncGrayscaleBase() const { return true; }
  void displayGrayscaleBase(RefreshMode, bool) { refreshHints.push_back(readerRefresh); }
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool) { refreshHints.push_back(readerRefresh); }
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  bool shouldSkipImageBlanking() const { return false; }
  bool supportsStripGrayscale() const { return true; }
  void cleanupGrayscaleBuffers(const uint8_t* buffer) {
    if (!buffer) ++canceled;
  }
};
