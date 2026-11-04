#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;      // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  const uint8_t* sourceData = nullptr;  // borrowed only for this decode
  size_t sourceSize = 0;
  std::string cachePath;  // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Decode callbacks can run for seconds on large, valid source images. Yield
  // occasionally so the watchdog's idle task can run without changing limits.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_WIDTH = 2048;
  // JPEGDEC streams scaled MCU blocks, so moderately wider JPEG sources are
  // safe as long as the total-pixel and heap guards still pass.
  static constexpr int MAX_JPEG_SOURCE_WIDTH = 4096;
  static constexpr int MAX_SOURCE_HEIGHT = 3072;
  static constexpr int64_t MAX_SOURCE_PIXELS = 2048LL * 3072LL;

  bool validateImageDimensions(int width, int height, const std::string& format, int maxSourceWidth = MAX_SOURCE_WIDTH);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
