#pragma once
#include <FontCacheManager.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Recording target/display adapter. Image rasterization uses the production
// DirectPixelWriter; this adapter only supplies geometry and owned buffers.
class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  int width, height, stride;
  Orientation orientation = Portrait;
  RenderMode mode = BW;
  bool deferredSupported = true;
  bool combinedBase = false;
  bool failBwBackup = false;
  bool absolute = false;
  std::vector<uint8_t> savedBw;
  bool supported = true, inverted = false, active = false;
  int origin = 0, rows = 0, placeholders = 0;
  uint8_t* target = nullptr;
  std::vector<uint8_t> bw, lsb, msb;
  std::vector<std::string> events;
  FontCacheManager fonts;
  GfxRenderer(int w = 792, int h = 481)
      : width(w), height(h), stride((w + 7) / 8), bw(stride * h, 0xA5), lsb(stride * h), msb(stride * h) {}
  int getDisplayWidth() const { return width; }
  int getDisplayHeight() const { return height; }
  int getDisplayWidthBytes() const { return stride; }
  int getScreenWidth() const { return orientation == Portrait || orientation == PortraitInverted ? height : width; }
  int getScreenHeight() const { return orientation == Portrait || orientation == PortraitInverted ? width : height; }
  Orientation getOrientation() const { return orientation; }
  RenderMode getRenderMode() const { return mode; }
  void setRenderMode(RenderMode m) {
    if (m == BW && absolute) {
      events.push_back("cancel-absolute");
      absolute = false;
    }
    mode = m;
  }
  bool grayPlanesAreAbsolute() const { return absolute; }
  uint8_t* getFrameBuffer() { return bw.data(); }
  bool supportsStripGrayscale() const { return supported && !inverted; }
  bool isStripTargetActive() const { return active; }
  int getWriteOriginY() const { return active ? origin : 0; }
  int getWriteRows() const { return active ? rows : height; }
  uint8_t* getWriteTarget() { return active ? target : bw.data(); }
  void beginStripTarget(uint8_t* p, int y, int count) {
    assert(!active && p && y >= 0 && count > 0 && y + count <= height);
    active = true;
    target = p;
    origin = y;
    rows = count;
  }
  void endStripTarget() {
    assert(active);
    active = false;
    target = nullptr;
    origin = rows = 0;
  }
  void clearScreen(uint8_t c) { memset(getWriteTarget(), c, size_t(stride) * getWriteRows()); }
  bool combinesGrayscaleBase() const { return combinedBase && !inverted; }
  void displayGrayscaleBase(int) { events.push_back("combined-base"); }
  bool supportsDeferredGrayscaleBase() const { return deferredSupported && !inverted; }
  void displayGrayscaleBaseAsync(int) { events.push_back("base"); }
  void copyGrayscalePlanes(const uint8_t* low, const uint8_t* high) {
    events.push_back("copy");
    memcpy(lsb.data(), low, lsb.size());
    memcpy(msb.data(), high, msb.size());
  }
  void waitRefreshComplete() { events.push_back("wait"); }
  void writeGrayscalePlaneStrip(bool low, const uint8_t* p, int y, int count) {
    assert(!active);
    events.push_back(low ? "lsb" : "msb");
    memcpy((low ? lsb : msb).data() + size_t(y) * stride, p, size_t(count) * stride);
  }
  void displayGrayBuffer() {
    assert(!active);
    events.push_back("gray");
    absolute = false;
  }
  void cleanupGrayscaleWithFrameBuffer() {
    assert(mode == BW && !active);
    events.push_back("cleanup");
  }
  bool storeBwBuffer() {
    if (failBwBackup) return false;
    savedBw = bw;
    return true;
  }
  void restoreBwBuffer() {
    bw = savedBw;
    savedBw.clear();
    cleanupGrayscaleWithFrameBuffer();
  }
  void copyGrayscaleLsbBuffers() { lsb = bw; }
  void copyGrayscaleMsbBuffers() { msb = bw; }
  FontCacheManager* getFontCacheManager() { return &fonts; }
  void preserveImagePolarity(int, int, int, int) {}
  bool isDisplayInverted() const { return inverted; }
  void fillRect(int, int, int, int, bool) { ++placeholders; }
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
    if (!active) return true;
    int lo, hi;
    switch (orientation) {
      case Portrait:
        lo = height - 1 - x1;
        hi = height - 1 - x0;
        break;
      case PortraitInverted:
        lo = x0;
        hi = x1;
        break;
      case LandscapeClockwise:
        lo = height - 1 - y1;
        hi = height - 1 - y0;
        break;
      default:
        lo = y0;
        hi = y1;
        break;
    }
    return hi >= origin && lo < origin + rows;
  }
};
