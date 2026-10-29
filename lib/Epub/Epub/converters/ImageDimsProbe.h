#pragma once

#include <Print.h>

#include "ImageToFramebufferDecoder.h"

// Streaming JPEG/PNG/GIF header parser. Feed bytes through Print; write() returns
// short once dimensions are known or the stream is unusable, allowing ZIP
// inflation to stop without extracting the complete image.
class ImageDimsProbe final : public Print {
 public:
  size_t write(uint8_t byte) override;
  size_t write(const uint8_t* data, size_t len) override;

  bool getDimensions(ImageDimensions& out) const;

 private:
  bool feed(uint8_t byte);

  enum class State : uint8_t {
    Sniff,
    PngHeader,
    GifHeader,
    JpegSoi,
    JpegFf,
    JpegMarker,
    JpegLenHi,
    JpegLenLo,
    JpegSkip,
    JpegSof,
    Done,
    Failed,
  };

  State state = State::Sniff;
  uint32_t position = 0;
  uint32_t skipLeft = 0;
  uint16_t segmentLength = 0;
  bool sofPending = false;
  uint8_t sofBytes[5] = {};
  uint8_t sofByteCount = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};
