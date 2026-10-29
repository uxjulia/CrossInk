#include "ImageDecoderFactory.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>
#include <string>

#include "GifToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
std::unique_ptr<GifToFramebufferConverter> ImageDecoderFactory::gifDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      if (!jpegDecoder) {
        LOG_ERR("DEC", "OOM: JPEG framebuffer decoder (%u free, %u max alloc)", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        return nullptr;
      }
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      if (!pngDecoder) {
        LOG_ERR("DEC", "OOM: PNG framebuffer decoder (%u free, %u max alloc)", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        return nullptr;
      }
    }
    return pngDecoder.get();
  } else if (GifToFramebufferConverter::supportsFormat(ext)) {
    if (!gifDecoder) {
      gifDecoder = makeUniqueNoThrow<GifToFramebufferConverter>();
      if (!gifDecoder) {
        LOG_ERR("DEC", "OOM: GIF decoder");
        return nullptr;
      }
    }
    return gifDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  return FsHelpers::hasJpgExtension(imagePath) || FsHelpers::hasPngExtension(imagePath) ||
         FsHelpers::hasGifExtension(imagePath);
}
