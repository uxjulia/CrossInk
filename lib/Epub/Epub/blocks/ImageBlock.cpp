#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height)
    : imagePath(std::move(imagePath)), sourcePath(std::move(sourcePath)), width(width), height(height) {}

void* ImageBlock::extractContext = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* context, ExtractFn fn) {
  extractContext = context;
  extractFn = fn;
}

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

void clampCachedRowsToLandscapeStrip(const GfxRenderer& renderer, const int imageY, int& rowStart, int& rowEnd) {
  if (!renderer.isStripTargetActive()) {
    return;
  }

  const int stripY0 = renderer.getWriteOriginY();
  const int stripY1Exclusive = stripY0 + renderer.getWriteRows();
  int logicalY0;
  int logicalY1Exclusive;

  switch (renderer.getOrientation()) {
    case GfxRenderer::LandscapeCounterClockwise:
      logicalY0 = stripY0;
      logicalY1Exclusive = stripY1Exclusive;
      break;
    case GfxRenderer::LandscapeClockwise:
      logicalY0 = renderer.getDisplayHeight() - stripY1Exclusive;
      logicalY1Exclusive = renderer.getDisplayHeight() - stripY0;
      break;
    default:
      return;
  }

  const int stripRowStart = logicalY0 - imageY;
  const int stripRowEnd = logicalY1Exclusive - imageY;
  if (rowStart < stripRowStart) rowStart = stripRowStart;
  if (rowEnd > stripRowEnd) rowEnd = stripRowEnd;
}

bool readValidCacheHeader(FsFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

// One full 2-bit PXC payload is retained for the current/last image. A full
// 800x480 image is 96 KB; cap pathological files at 128 KB. The buffer is PSRAM
// only, so C3 keeps the existing ~4 KB streamed reader and internal heap budget.
constexpr size_t MAX_RETAINED_PXC_BYTES = 128 * 1024;
HeapByteBuffer retainedPxcPixels;
size_t retainedPxcCapacity = 0;
uint16_t retainedPxcWidth = 0;
uint16_t retainedPxcHeight = 0;
std::string retainedPxcPath;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

bool renderCachedPixels(GfxRenderer& renderer, const uint8_t* pixels, const uint16_t cachedWidth,
                        const uint16_t cachedHeight, const int x, const int y) {
  if (!pixels) return false;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = x < 0 ? -x : 0;
  int clipYStart = y < 0 ? -y : 0;
  int clipXEnd = std::min<int>(cachedWidth, screenWidth - x);
  int clipYEnd = std::min<int>(cachedHeight, screenHeight - y);
  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) return true;

  int renderRowStart = clipYStart;
  int renderRowEnd = clipYEnd;
  clampCachedRowsToLandscapeStrip(renderer, y, renderRowStart, renderRowEnd);
  if (renderRowStart >= renderRowEnd) return true;

  const int bytesPerRow = (cachedWidth + 3) / 4;
  DirectPixelWriter pw;
  pw.init(renderer);
  for (int row = renderRowStart; row < renderRowEnd; ++row) {
    const uint8_t* rowBuffer = pixels + static_cast<size_t>(row) * bytesPerRow;
    pw.beginRow(y + row);
    for (int col = clipXStart; col < clipXEnd; ++col) {
      const int byteIdx = col >> 2;
      const int bitShift = 6 - (col & 3) * 2;
      pw.writePixel(x + col, (rowBuffer[byteIdx] >> bitShift) & 0x03);
    }
  }
  return true;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  const bool retainedDimensionsMatch = abs(static_cast<int>(retainedPxcWidth) - expectedWidth) <= 1 &&
                                       abs(static_cast<int>(retainedPxcHeight) - expectedHeight) <= 1;
  if (retainedPxcPixels && retainedPxcPath == cachePath && retainedDimensionsMatch) {
    return renderCachedPixels(renderer, retainedPxcPixels.get(), retainedPxcWidth, retainedPxcHeight, x, y);
  }

  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  const size_t bytesPerRow = (cachedWidth + 3U) / 4U;
  const size_t pixelBytes = bytesPerRow * cachedHeight;
  if (psramHeapAvailable() && pixelBytes <= MAX_RETAINED_PXC_BYTES) {
    if (retainedPxcCapacity < pixelBytes) {
      auto grown = makePsramByteBufferNoThrow(pixelBytes);
      if (grown) {
        retainedPxcPixels = std::move(grown);
        retainedPxcCapacity = pixelBytes;
      }
    }
    // A failed growth leaves the previous, smaller buffer in place; reading the
    // new payload into it would overflow that allocation.
    if (retainedPxcPixels && retainedPxcCapacity >= pixelBytes && cacheFile.seek(4) &&
        cacheFile.read(retainedPxcPixels.get(), pixelBytes) == static_cast<int>(pixelBytes)) {
      retainedPxcPath = cachePath;
      retainedPxcWidth = cachedWidth;
      retainedPxcHeight = cachedHeight;
      cacheFile.close();
      LOG_INF("EPS", "Retained PXC in PSRAM: bytes=%u dimensions=%ux%u", static_cast<unsigned>(pixelBytes), cachedWidth,
              cachedHeight);
      MemoryBudget::logEpubHeapPools("pxc retained");
      return renderCachedPixels(renderer, retainedPxcPixels.get(), cachedWidth, cachedHeight, x, y);
    }
    retainedPxcPath.clear();
    if (!cacheFile.seek(4)) {
      cacheFile.close();
      return false;
    }
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = 0;
  int clipYStart = 0;
  int clipXEnd = cachedWidth;
  int clipYEnd = cachedHeight;
  if (x < 0) clipXStart = -x;
  if (y < 0) clipYStart = -y;
  if (screenWidth - x < clipXEnd) clipXEnd = screenWidth - x;
  if (screenHeight - y < clipYEnd) clipYEnd = screenHeight - y;

  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    LOG_DBG("IMG", "Cached image is outside screen after clipping");
    cacheFile.close();
    return true;
  }

  int renderRowStart = clipYStart;
  int renderRowEnd = clipYEnd;
  clampCachedRowsToLandscapeStrip(renderer, y, renderRowStart, renderRowEnd);
  if (renderRowStart >= renderRowEnd) {
    cacheFile.close();
    return true;
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRowInt = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const int rowsToRender = renderRowEnd - renderRowStart;
  int rowsPerRead = 4096 / bytesPerRowInt;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRowInt);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRowInt);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  const size_t dataOffset = 4U + static_cast<size_t>(renderRowStart) * static_cast<size_t>(bytesPerRowInt);
  if (!cacheFile.seek(dataOffset)) {
    LOG_ERR("IMG", "Cache seek error at row %d", renderRowStart);
    free(readBuffer);
    cacheFile.close();
    return false;
  }

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = renderRowStart; row < renderRowEnd; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (renderRowEnd - row < rowsPerRead) ? (renderRowEnd - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRowInt;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRowInt;
    bufferRow++;

    if (row < clipYStart) continue;
    if (row >= clipYEnd) break;

    const int destY = y + row;
    pw.beginRow(destY);
    // Walk only the on-screen columns: writePixel drops off-band rows but does
    // not clip X, so this range is what keeps a partially off-screen image
    // inside the framebuffer.
    for (int col = clipXStart; col < clipXEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  cacheFile.close();
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  const bool valid = readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
  cacheFile.close();
  return valid;
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() {
  failedImageCount = 0;
  releaseSessionPixelCache();
}

void ImageBlock::releaseSessionPixelCache() {
  retainedPxcPixels.reset();
  retainedPxcCapacity = 0;
  retainedPxcWidth = 0;
  retainedPxcHeight = 0;
  retainedPxcPath.clear();
}

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack) const {
  renderer.fillRect(x, y, width, height, foregroundBlack);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, !foregroundBlack);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image size: %dx%d", width, height);
    return;
  }

  // Reject only fully off-screen images. Decoders and cache rendering clip
  // partially visible images to the logical screen bounds.
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }
  const bool fullyOnScreen = x >= 0 && y >= 0 && x + width <= screenWidth && y + height <= screenHeight;

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    renderer.preserveImagePolarity(x, y, width, height);
    return;  // Successfully rendered from cache
  }

  if (!sourcePath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    if (!extractFn(extractContext, sourcePath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", sourcePath.c_str());
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  if (fullyOnScreen) {
    config.cachePath = cachePath;  // Enable caching during decode
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  renderer.preserveImagePolarity(x, y, width, height);
}

bool ImageBlock::serialize(FsFile& file) {
  return serialization::tryWriteString(file, imagePath) && serialization::tryWriteString(file, sourcePath) &&
         serialization::tryWritePod(file, width) && serialization::tryWritePod(file, height);
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  if (!serialization::tryReadString(file, path)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image path");
    return nullptr;
  }
  std::string source;
  if (!serialization::tryReadString(file, source)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image source path");
    return nullptr;
  }
  int16_t w, h;
  if (!serialization::tryReadPod(file, w) || !serialization::tryReadPod(file, h)) {
    LOG_ERR("IMG", "Deserialization failed: truncated image metadata");
    return nullptr;
  }

  auto* imageBlock = new (std::nothrow) ImageBlock(std::move(path), std::move(source), w, h);
  if (!imageBlock) {
    LOG_ERR("IMG", "Deserialization failed: could not allocate ImageBlock");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(imageBlock);
}
