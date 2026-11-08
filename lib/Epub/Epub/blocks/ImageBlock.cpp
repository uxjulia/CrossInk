#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PrintSerialization.h>
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
ImageBlock::SeedCacheFn ImageBlock::seedCacheFn = nullptr;

ImageBlock::LoadFn ImageBlock::loadFn = nullptr;

void ImageBlock::setExtractor(void* context, ExtractFn extract, SeedCacheFn seedCache, LoadFn load) {
  loadFn = load;
  extractContext = context;
  extractFn = extract;
  seedCacheFn = seedCache;
}

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Revision 2 rebuilds decoded pixels after the progressive JPEG correction.
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + "_v2.pxc";
  }
  return imagePath + "_v2.pxc";
}

// Half-open image-local bounds shared by retained and streamed PXC rendering.
// Portrait strips restrict columns; landscape strips restrict rows.
struct CachedImageClip {
  int x0, y0, x1, y1;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
};

CachedImageClip cachedImageClip(const GfxRenderer& renderer, int x, int y, int width, int height) {
  CachedImageClip clip{std::max(0, -x), std::max(0, -y), std::min(width, renderer.getScreenWidth() - x),
                       std::min(height, renderer.getScreenHeight() - y)};
  if (!renderer.isStripTargetActive()) return clip;

  const int s0 = renderer.getWriteOriginY();
  const int s1 = s0 + renderer.getWriteRows();
  const int h = renderer.getDisplayHeight();
  switch (renderer.getOrientation()) {
    case GfxRenderer::LandscapeCounterClockwise:
      clip.y0 = std::max(clip.y0, s0 - y);
      clip.y1 = std::min(clip.y1, s1 - y);
      break;
    case GfxRenderer::LandscapeClockwise:
      clip.y0 = std::max(clip.y0, h - s1 - y);
      clip.y1 = std::min(clip.y1, h - s0 - y);
      break;
    case GfxRenderer::Portrait:
      clip.x0 = std::max(clip.x0, h - s1 - x);
      clip.x1 = std::min(clip.x1, h - s0 - x);
      break;
    case GfxRenderer::PortraitInverted:
      clip.x0 = std::max(clip.x0, s0 - x);
      clip.x1 = std::min(clip.x1, s1 - x);
      break;
  }
  return clip;
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

// Retain up to two recently rendered 2-bit PXC payloads. Their allocated
// capacities share this 128 KB budget (a full 800x480 payload is 96 KB), so
// alternating images avoid repeated SD reads without adding PSRAM pressure.
// PSRAM-only ownership leaves C3 on the existing small streamed-reader path.
constexpr size_t MAX_RETAINED_PXC_BYTES = 128 * 1024;
constexpr size_t RETAINED_PXC_ENTRY_COUNT = 2;

struct RetainedPxcEntry {
  HeapByteBuffer pixels;
  size_t capacity = 0;
  size_t payloadBytes = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t lastUse = 0;
  std::string path;
};

RetainedPxcEntry retainedPxcEntries[RETAINED_PXC_ENTRY_COUNT];
// Kept separately for aggregate-budget checks and host assertions. It counts
// allocated capacity, never just active payload bytes.
size_t retainedPxcCapacity = 0;
uint32_t retainedPxcUseClock = 0;

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

void clearRetainedPxcEntry(RetainedPxcEntry& entry) {
  retainedPxcCapacity -= entry.capacity;
  entry.pixels.reset();
  entry.capacity = 0;
  entry.payloadBytes = 0;
  entry.width = 0;
  entry.height = 0;
  entry.lastUse = 0;
  entry.path.clear();
}

void forgetRetainedPxcContent(RetainedPxcEntry& entry) {
  entry.payloadBytes = 0;
  entry.width = 0;
  entry.height = 0;
  entry.lastUse = 0;
  entry.path.clear();
}

void invalidateRetainedPxcPath(const std::string& cachePath) {
  for (auto& entry : retainedPxcEntries) {
    if (entry.payloadBytes != 0 && entry.path == cachePath) clearRetainedPxcEntry(entry);
  }
}

void touchRetainedPxcEntry(RetainedPxcEntry& entry) { entry.lastUse = ++retainedPxcUseClock; }

RetainedPxcEntry* findRetainedPxcEntry(const std::string& cachePath, const int expectedWidth,
                                       const int expectedHeight) {
  for (auto& entry : retainedPxcEntries) {
    if (entry.payloadBytes != 0 && entry.path == cachePath && abs(entry.width - expectedWidth) <= 1 &&
        abs(entry.height - expectedHeight) <= 1) {
      return &entry;
    }
  }
  return nullptr;
}

RetainedPxcEntry* leastRecentlyUsedEntry(const size_t minimumCapacity = 0) {
  RetainedPxcEntry* result = nullptr;
  for (auto& entry : retainedPxcEntries) {
    if (entry.capacity < minimumCapacity) continue;
    if (!result || entry.lastUse < result->lastUse) result = &entry;
  }
  return result;
}

bool allocateRetainedPxcEntry(RetainedPxcEntry& entry, const size_t pixelBytes) {
  // Keep enough PSRAM for the next JPEG decoder as well as the normal EPUB
  // reserve; otherwise a retained PXC can starve a later chapter image.
  const size_t requiredPsram = pixelBytes + MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  if (!MemoryBudget::canAllocatePsram(requiredPsram)) return false;
  entry.pixels = makePsramByteBufferNoThrow(pixelBytes);
  if (!entry.pixels) {
    LOG_ERR("IMG", "Failed to retain PXC in PSRAM: %u bytes", static_cast<unsigned>(pixelBytes));
    return false;
  }
  entry.capacity = pixelBytes;
  retainedPxcCapacity += pixelBytes;
  return true;
}

RetainedPxcEntry* prepareRetainedPxcEntry(const size_t pixelBytes) {
  if (!psramHeapAvailable() || pixelBytes > MAX_RETAINED_PXC_BYTES) return nullptr;

  RetainedPxcEntry* empty = nullptr;
  for (auto& candidate : retainedPxcEntries) {
    if (candidate.capacity == 0) {
      empty = &candidate;
      break;
    }
  }

  // Fill a budget-fitting empty slot before reusing another image's backing
  // buffer, so two alternating images remain resident.
  if (empty && pixelBytes <= MAX_RETAINED_PXC_BYTES - retainedPxcCapacity) {
    return allocateRetainedPxcEntry(*empty, pixelBytes) ? empty : nullptr;
  }

  // Reuse an LRU allocation when it is already large enough. This avoids heap
  // churn while evicting only cached content, not its PSRAM backing buffer.
  if (auto* reusable = leastRecentlyUsedEntry(pixelBytes)) {
    forgetRetainedPxcContent(*reusable);
    return reusable;
  }

  // Both slots are occupied by smaller buffers. Evict the LRU even when the
  // aggregate budget has room, so the new image can use its slot.
  if (!empty) {
    empty = leastRecentlyUsedEntry(1);
    if (!empty) return nullptr;
    clearRetainedPxcEntry(*empty);
  }

  // A larger image may need to evict both entries before its own fallible
  // PSRAM allocation fits the aggregate 128 KB budget.
  while (pixelBytes > MAX_RETAINED_PXC_BYTES - retainedPxcCapacity) {
    auto* victim = leastRecentlyUsedEntry(1);
    if (!victim) return nullptr;
    clearRetainedPxcEntry(*victim);
  }
  for (auto& candidate : retainedPxcEntries) {
    if (candidate.capacity == 0) {
      empty = &candidate;
      break;
    }
  }
  if (!empty) return nullptr;
  return allocateRetainedPxcEntry(*empty, pixelBytes) ? empty : nullptr;
}

bool renderCachedPixels(GfxRenderer& renderer, const uint8_t* pixels, const uint16_t cachedWidth,
                        const uint16_t cachedHeight, const int x, const int y, const bool imageGrayscale) {
  if (!pixels) return false;
  const auto clip = cachedImageClip(renderer, x, y, cachedWidth, cachedHeight);
  if (clip.empty()) return true;

  const int bytesPerRow = (cachedWidth + 3) / 4;
  DirectPixelWriter pw;
  pw.init(renderer, imageGrayscale);
  for (int row = clip.y0; row < clip.y1; ++row) {
    const uint8_t* rowBuffer = pixels + static_cast<size_t>(row) * bytesPerRow;
    pw.beginRow(y + row);
    for (int col = clip.x0; col < clip.x1; ++col) {
      const int byteIdx = col >> 2;
      const int bitShift = 6 - (col & 3) * 2;
      pw.writePixel(x + col, (rowBuffer[byteIdx] >> bitShift) & 0x03);
    }
  }
  return true;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, const bool imageGrayscale = true) {
  // Cache generation invalidates retained entries before overwriting a PXC
  // path, so repeated grayscale strips can avoid storage I/O entirely.
  if (auto* retained = findRetainedPxcEntry(cachePath, expectedWidth, expectedHeight)) {
    touchRetainedPxcEntry(*retained);
    return renderCachedPixels(renderer, retained->pixels.get(), retained->width, retained->height, x, y,
                              imageGrayscale);
  }

  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    invalidateRetainedPxcPath(cachePath);
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    invalidateRetainedPxcPath(cachePath);
    cacheFile.close();
    return false;
  }

  const auto clip = cachedImageClip(renderer, x, y, cachedWidth, cachedHeight);
  if (clip.empty()) {
    cacheFile.close();
    return true;
  }

  const size_t bytesPerRow = (cachedWidth + 3U) / 4U;
  const size_t pixelBytes = bytesPerRow * cachedHeight;

  if (psramHeapAvailable() && pixelBytes <= MAX_RETAINED_PXC_BYTES) {
    RetainedPxcEntry* retained = prepareRetainedPxcEntry(pixelBytes);
    if (retained && cacheFile.seek(4) &&
        cacheFile.read(retained->pixels.get(), pixelBytes) == static_cast<int>(pixelBytes)) {
      retained->payloadBytes = pixelBytes;
      retained->width = cachedWidth;
      retained->height = cachedHeight;
      retained->path = cachePath;
      touchRetainedPxcEntry(*retained);
      cacheFile.close();
      LOG_INF("EPS", "Retained PXC in PSRAM: bytes=%u total=%u entries=2 dimensions=%ux%u",
              static_cast<unsigned>(pixelBytes), static_cast<unsigned>(retainedPxcCapacity), cachedWidth, cachedHeight);
      MemoryBudget::logEpubHeapPools("pxc retained");
      return renderCachedPixels(renderer, retained->pixels.get(), cachedWidth, cachedHeight, x, y, imageGrayscale);
    }
    // A short read must never leave an entry that could be mistaken for a
    // complete cache payload. Keep its allocation for a later retry.
    if (retained) forgetRetainedPxcContent(*retained);
    if (!cacheFile.seek(4)) {
      cacheFile.close();
      return false;
    }
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRowInt = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const int rowsToRender = clip.y1 - clip.y0;
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
  pw.init(renderer, imageGrayscale);

  const size_t dataOffset = 4U + static_cast<size_t>(clip.y0) * static_cast<size_t>(bytesPerRowInt);
  if (!cacheFile.seek(dataOffset)) {
    LOG_ERR("IMG", "Cache seek error at row %d", clip.y0);
    free(readBuffer);
    cacheFile.close();
    return false;
  }

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = clip.y0; row < clip.y1; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (clip.y1 - row < rowsPerRead) ? (clip.y1 - row) : rowsPerRead;
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

    const int destY = y + row;
    pw.beginRow(destY);
    // Clip before unpacking, including portrait strip columns. Row reads stay
    // sequential and batched; this reduces pixel work, not portrait SD bytes.
    for (int col = clip.x0; col < clip.x1; col++) {
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

void ImageBlock::prepareCache() const {
  if (hasValidCache()) {
    LOG_DBG("IMG", "Local image cache hit: %s", imagePath.c_str());
    return;
  }
  if (sourcePath.empty()) return;
  const std::string cache = getCachePath(imagePath);
  // Seed/extract replaces this path's PXC payload. Invalidate before it is
  // regenerated so a retained previous payload cannot outlive that write.
  invalidateRetainedPxcPath(cache);
  Storage.remove((cache + ".optimizer.tmp").c_str());
  Storage.remove((cache + ".optimizer.source").c_str());
  if (seedCacheFn && seedCacheFn(extractContext, sourcePath.c_str(), width, height, cache.c_str())) return;
  // JPEG/PNG may decode straight from PSRAM in render(); GIF keeps its file path.
  if (loadFn && psramHeapAvailable() && ImageDecoderFactory::supportsMemorySource(imagePath)) return;
  if (extractFn && !Storage.exists(imagePath.c_str()) &&
      !extractFn(extractContext, sourcePath.c_str(), imagePath.c_str())) {
    LOG_ERR("IMG", "Image preflight extraction failed: %s", sourcePath.c_str());
  }
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() {
  failedImageCount = 0;
  releaseSessionPixelCache();
}

void ImageBlock::releaseSessionPixelCache() {
  retainedPxcCapacity = 0;
  retainedPxcUseClock = 0;
  for (auto& entry : retainedPxcEntries) {
    entry.pixels.reset();
    entry.capacity = 0;
    entry.payloadBytes = 0;
    entry.width = 0;
    entry.height = 0;
    entry.lastUse = 0;
    entry.path.clear();
  }
}

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack) const {
  renderer.fillRect(x, y, width, height, foregroundBlack);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, !foregroundBlack);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack,
                        const bool imageGrayscale) {
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
  if (renderFromCache(renderer, cachePath, x, y, width, height, imageGrayscale)) {
    renderer.preserveImagePolarity(x, y, width, height);
    return;  // Successfully rendered from cache
  }

  // Retain only this image's compressed source until its decoder returns.
  HeapByteBuffer source;
  size_t sourceSize = 0;
  if (!sourcePath.empty() && !Storage.exists(imagePath.c_str())) {
    if (loadFn && ImageDecoderFactory::supportsMemorySource(imagePath))
      source = loadFn(extractContext, sourcePath.c_str(), sourceSize);
    if (!source && extractFn && !extractFn(extractContext, sourcePath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Image extraction failed: %s", sourcePath.c_str());
      rememberImageFailure(imagePath);
      renderPlaceholder(renderer, x, y, foregroundBlack);
      return;
    }
  }
  if (!source) {
    FsFile file;
    const bool opened = Storage.openFileForRead("IMG", imagePath, file);
    const size_t fileSize = opened ? file.size() : 0;
    file.close();
    if (fileSize == 0) {
      LOG_ERR("IMG", "Missing or empty image: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      renderPlaceholder(renderer, x, y, foregroundBlack);
      return;
    }
  }

  RenderConfig config;
  config.sourceData = source.get();
  config.sourceSize = sourceSize;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = imageGrayscale;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  if (fullyOnScreen) {
    // The decoder will replace this PXC path. Drop any retained payload first
    // so the next grayscale pass cannot draw its previous generation.
    invalidateRetainedPxcPath(cachePath);
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

bool ImageBlock::serialize(Print& file) {
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
