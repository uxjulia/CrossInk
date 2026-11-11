#pragma once

#include <cstdint>
#include <vector>

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_MSB, GRAYSCALE_LSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  int getScreenWidth() const { return width; }
  int getScreenHeight() const { return height; }
  int getDisplayWidth() const { return width; }
  int getDisplayHeight() const { return height; }
  uint16_t getDisplayWidthBytes() const { return static_cast<uint16_t>((width + 7) / 8); }
  uint8_t* getWriteTarget() { return framebuffer.data(); }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return height; }
  RenderMode getRenderMode() const { return BW; }
  bool isDisplayInverted() const { return false; }
  bool grayPlanesAreAbsolute() const { return false; }
  Orientation getOrientation() const { return LandscapeCounterClockwise; }

 private:
  static constexpr int width = 800;
  static constexpr int height = 480;
  std::vector<uint8_t> framebuffer = std::vector<uint8_t>((width + 7) / 8 * height, 0xff);
};
