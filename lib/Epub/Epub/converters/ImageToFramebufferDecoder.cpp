#include "ImageToFramebufferDecoder.h"

#include <Logging.h>

bool ImageToFramebufferDecoder::validateImageDimensions(int width, int height, const std::string& format) {
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (width <= 0 || height <= 0 || width > MAX_SOURCE_WIDTH || height > MAX_SOURCE_HEIGHT ||
      pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "Image too large or invalid (%dx%d %s, pixels=%lld), max supported: %dx%d / %lld pixels", width,
            height, format.c_str(), static_cast<long long>(pixels), MAX_SOURCE_WIDTH, MAX_SOURCE_HEIGHT,
            static_cast<long long>(MAX_SOURCE_PIXELS));
    return false;
  }
  return true;
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
