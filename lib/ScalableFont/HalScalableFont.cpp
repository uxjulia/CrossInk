#include "HalScalableFont.h"
#if CROSSINK_SCALABLE_FONTS
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/task.h>
#ifndef SIMULATOR
#include <Arduino.h>
#include <esp_task_wdt.h>
#endif

#include <algorithm>
#include <cstring>
#include <new>

#include "FixedArenaAllocator.h"
#include "FontByteDigest.h"
#include "ScalableFontSizing.h"
#include "TrueTypeFormat.h"

namespace {
SemaphoreHandle_t renderMutex = nullptr;
#ifdef SIMULATOR
constexpr auto Pool = MemoryPool::None;
#else
constexpr auto Pool = MemoryPool::Psram;
#endif
// The eight built-in faces and four streamed ChareInk faces need more than
// 1 MiB in a warm preview; retain smaller arenas for fragmented PSRAM heaps.
constexpr size_t WorkspaceOptions[] = {1280 * 1024, 1024 * 1024, 768 * 1024};
constexpr size_t PixelBytes = 512 * 1024;
constexpr size_t PixelCacheSlots = 512;
constexpr size_t MetricCacheSlots = 512;
constexpr size_t KerningCacheSlots = 512;
constexpr uint32_t AdapterRevision = 9;
constexpr uint32_t SfntChecksumMagic = 0xB1B0AFBAu;
// Unicode ends at 0x10FFFF, so the high bit safely marks a shaped glyph ID in
// EpdGlyph::dataOffset without adding renderer-specific state to the SDK.
constexpr uint32_t GlyphIdMarker = 0x80000000u;

constexpr size_t alignSize(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

int32_t round26_6To16(int32_t value) {
  const int64_t wide = value;
  return int32_t(wide >= 0 ? (wide + 2) / 4 : -((-wide + 2) / 4));
}
int ceil26_6(int32_t value) {
  const int64_t wide = value;
  return int(wide >= 0 ? (wide + 63) / 64 : -((-wide) / 64));
}
int floor26_6(int32_t value) {
  const int64_t wide = value;
  return int(wide >= 0 ? wide / 64 : -((-wide + 63) / 64));
}

const char* glyphFailureName(const freeink::font::FtFont::GlyphFailure failure) {
  using Failure = freeink::font::FtFont::GlyphFailure;
  switch (failure) {
    case Failure::MissingGlyph:
      return "missing-glyph";
    case Failure::Size:
      return "set-size";
    case Failure::Load:
      return "load-glyph";
    case Failure::Embolden:
      return "embolden";
    case Failure::Render:
      return "render";
    case Failure::Bounds:
      return "bounds";
    case Failure::BitmapBuffer:
      return "bitmap-buffer";
    case Failure::None:
      return "adapter-bounds";
  }
  return "unknown";
}

const char* initFailureName(const freeink::font::FtFont::InitFailure failure) {
  using Failure = freeink::font::FtFont::InitFailure;
  switch (failure) {
    case Failure::Library:
      return "library";
    case Failure::Source:
      return "source";
    case Failure::Allocation:
      return "allocation";
    case Failure::OpenFace:
      return "open-face";
    case Failure::SetSize:
      return "set-size";
    case Failure::None:
      return "none";
  }
  return "unknown";
}

struct PixelEntry {
  uint64_t key = 0;
  size_t offset = 0;
};

struct MetricEntry {
  uint64_t key = 0;
  EpdGlyph glyph{};
};

struct KerningEntry {
  uint64_t leftKey = 0;
  uint32_t rightGlyph = 0;
  int8_t value = 0;
};

constexpr size_t PixelTableBytes = alignSize(sizeof(PixelEntry) * PixelCacheSlots, alignof(std::max_align_t));
constexpr size_t MetricTableBytes = alignSize(sizeof(MetricEntry) * MetricCacheSlots, alignof(std::max_align_t));
constexpr size_t KerningTableBytes = alignSize(sizeof(KerningEntry) * KerningCacheSlots, alignof(std::max_align_t));
constexpr size_t CacheTableBytes = PixelTableBytes + MetricTableBytes + KerningTableBytes;
constexpr size_t PixelArenaBytes = CacheTableBytes + PixelBytes;
struct Runtime;
Runtime* liveRuntime = nullptr;
struct Runtime {
  Runtime() { liveRuntime = this; }
  ~Runtime() { liveRuntime = nullptr; }
  HeapByteBuffer workspace;
  FontLoadError beginError = FontLoadError::None;
  HeapByteBuffer pixels;
  FixedArenaAllocator allocator;
  PixelEntry* pixelEntries = nullptr;
  MetricEntry* metricEntries = nullptr;
  KerningEntry* kerningEntries = nullptr;
  uint8_t* pixelData = nullptr;
  size_t pixelUsed = 0;
  uint32_t nextCacheId = 1;
  uint16_t failedMetricWidth = 0;
  uint16_t failedMetricHeight = 0;
  uint16_t lastBitmapWidth = 0;
  uint16_t lastBitmapHeight = 0;

  static uint64_t cacheKey(const uint32_t cacheId, const uint32_t glyphOrCodepoint, const uint8_t points) {
    return (uint64_t(cacheId) << 40) | (uint64_t(points) << 32) | glyphOrCodepoint;
  }

  static size_t cacheIndex(const uint32_t cacheId, const uint32_t glyphOrCodepoint, const uint8_t points,
                           const size_t slots) {
    return (glyphOrCodepoint * 31u + points * 7u + cacheId) % slots;
  }

  void clearPixels() {
    pixelUsed = 0;
    if (pixelEntries)
      for (size_t i = 0; i < PixelCacheSlots; ++i) pixelEntries[i] = PixelEntry{};
  }

  void clearMetrics() {
    if (metricEntries)
      for (size_t i = 0; i < MetricCacheSlots; ++i) metricEntries[i] = MetricEntry{};
    if (kerningEntries)
      for (size_t i = 0; i < KerningCacheSlots; ++i) kerningEntries[i] = KerningEntry{};
  }

  void clearCaches() {
    clearPixels();
    clearMetrics();
  }

  bool glyphMetrics(freeink::font::FtFont& font, const uint32_t cacheId, const uint32_t glyphOrCodepoint,
                    const uint8_t points, EpdGlyph& glyph) {
    const uint64_t key = cacheKey(cacheId, glyphOrCodepoint, points);
    auto& entry = metricEntries[cacheIndex(cacheId, glyphOrCodepoint, points, MetricCacheSlots)];
    if (entry.key == key) {
      glyph = entry.glyph;
      return true;
    }

    freeink::font::FtFont::GlyphMetrics metrics;
    allocator.clearFailure();
    const bool measured =
        glyphOrCodepoint & GlyphIdMarker
            ? font.metricsGlyph26_6(glyphOrCodepoint & ~GlyphIdMarker, scalableFontPixelSize26_6(points), metrics)
            : font.metrics26_6(glyphOrCodepoint, scalableFontPixelSize26_6(points), metrics);
    failedMetricWidth = metrics.width;
    failedMetricHeight = metrics.height;
    if (!measured || metrics.width > UINT8_MAX || metrics.height > UINT8_MAX) return false;

    const auto advance16 = uint16_t(std::clamp<int32_t>(round26_6To16(metrics.advance26_6), 0, UINT16_MAX));
    glyph = {uint8_t(metrics.width),
             uint8_t(metrics.height),
             advance16,
             metrics.left,
             metrics.top,
             uint16_t((unsigned(metrics.width) * metrics.height + 3) / 4),
             glyphOrCodepoint};
    entry = {key, glyph};
    return true;
  }

  int8_t kerning(freeink::font::FtFont& font, const uint32_t cacheId, const uint32_t leftGlyph,
                 const uint32_t rightGlyph, const uint8_t points) {
    if (!leftGlyph || !rightGlyph) return 0;
    const uint64_t leftKey = cacheKey(cacheId, leftGlyph, points);
    auto& entry = kerningEntries[(leftGlyph * 31u + rightGlyph * 17u + points * 7u + cacheId) % KerningCacheSlots];
    if (entry.leftKey == leftKey && entry.rightGlyph == rightGlyph) return entry.value;

    const auto measured = font.kerningGlyphs26_6(leftGlyph, rightGlyph, scalableFontPixelSize26_6(points));
    const auto value = int8_t(std::clamp<int32_t>(round26_6To16(measured), -128, 127));
    entry = {leftKey, rightGlyph, value};
    return value;
  }

  const uint8_t* packedBitmap(freeink::font::FtFont& font, const uint32_t cacheId, uint32_t glyphOrCodepoint,
                              uint8_t points, uint8_t width, uint8_t height) {
    const uint64_t key = cacheKey(cacheId, glyphOrCodepoint, points);
    auto& entry = pixelEntries[cacheIndex(cacheId, glyphOrCodepoint, points, PixelCacheSlots)];
    if (entry.key == key) return pixelData + entry.offset;

    allocator.clearFailure();
    const auto* bitmap =
        glyphOrCodepoint & GlyphIdMarker
            ? font.rasterizeGlyph26_6(glyphOrCodepoint & ~GlyphIdMarker, scalableFontPixelSize26_6(points))
            : font.rasterize26_6(glyphOrCodepoint, scalableFontPixelSize26_6(points));
    lastBitmapWidth = bitmap ? bitmap->width : 0;
    lastBitmapHeight = bitmap ? bitmap->height : 0;
    if (!bitmap || bitmap->width != width || bitmap->height != height) return nullptr;
    const size_t bytes = (size_t(width) * height + 3) / 4;
    if (!bytes || bytes > PixelBytes) return nullptr;
    if (pixelUsed + bytes > PixelBytes) clearPixels();

    auto* dest = pixelData + pixelUsed;
    std::memset(dest, 0, bytes);
    for (unsigned y = 0; y < bitmap->height; ++y) {
      const uint8_t* row = bitmap->pixels + size_t(y) * bitmap->width;
      for (unsigned x = 0; x < bitmap->width; ++x) {
        const unsigned value = row[x] >> 4;
        // Match the default .cpfont converter's 4/8/12 cutoffs so the same
        // outline keeps its stroke weight when installed in either format.
        const uint8_t coverage = value < 4 ? 0 : value < 8 ? 1 : value < 12 ? 2 : 3;
        const size_t index = size_t(y) * bitmap->width + x;
        dest[index / 4] |= coverage << (6 - (index % 4) * 2);
      }
    }
    entry = {key, pixelUsed};
    pixelUsed += bytes;
    return dest;
  }

  bool begin() {
    if (workspace) return true;
    beginError = FontLoadError::Memory;
    // Persistent PSRAM arenas bound all FreeType scratch and cached coverage;
    // neither belongs on the render stack or in scarce internal DRAM.
    HeapByteBuffer w;
    HeapByteBuffer p;
    size_t workspaceBytes = 0;
    for (const size_t candidate : WorkspaceOptions) {
      w = makeAlignedByteBufferNoThrow(candidate, Pool);
      if (w) p = makeAlignedByteBufferNoThrow(PixelArenaBytes, Pool);
      if (w && p) {
        workspaceBytes = candidate;
        break;
      }
      w.reset();
      p.reset();
    }
    if (workspaceBytes && workspaceBytes != WorkspaceOptions[0])
      LOG_DBG("TTF", "Using %u-byte fallback font arena", unsigned(workspaceBytes));
    if (!w || !p || !allocator.initialize(w.get(), workspaceBytes)) {
      const auto heap = byteHeapSnapshot(Pool);
      LOG_ERR("TTF", "Cannot allocate font arenas (tried %u/%u/%u + %u bytes; pool free=%u largest=%u)",
              unsigned(WorkspaceOptions[0]), unsigned(WorkspaceOptions[1]), unsigned(WorkspaceOptions[2]),
              unsigned(PixelArenaBytes), unsigned(heap.free), unsigned(heap.largest));
      return false;
    }
    workspace = std::move(w);
    freeink::font::FtFont::MemoryCallbacks memory{&allocator, FixedArenaAllocator::allocate,
                                                  FixedArenaAllocator::deallocate, FixedArenaAllocator::reallocate};
    if (!freeink::font::FtFont::configureMemory(&memory)) {
      beginError = FontLoadError::Read;
      LOG_ERR("TTF", "Cannot configure bounded font allocator");
      workspace.reset();
      return false;
    }
    pixels = std::move(p);
    beginError = FontLoadError::None;
    pixelEntries = reinterpret_cast<PixelEntry*>(pixels.get());
    metricEntries = reinterpret_cast<MetricEntry*>(pixels.get() + PixelTableBytes);
    kerningEntries = reinterpret_cast<KerningEntry*>(pixels.get() + PixelTableBytes + MetricTableBytes);
    pixelData = pixels.get() + CacheTableBytes;
    clearCaches();
    return true;
  }
};
Runtime& runtime() {
  static Runtime value;
  return value;
}
truetype::Result inspectFormat(HalFile& file, size_t size) {
  return truetype::inspect(size, [&](size_t offset, uint8_t* bytes, size_t count) {
    return file.seekSet(offset) && file.read(bytes, count) == static_cast<int>(count);
  });
}
bool validFileSize(const char* path, size_t size, size_t limit = HalScalableFont::MaxFileBytes) {
  if (size >= 12 && size <= limit) return true;
  LOG_ERR("TTF", "Unsupported TTF file size: %s (%u bytes; limit=%u)", path, unsigned(size), unsigned(limit));
  return false;
}
struct FontByteSummary {
  uint32_t contentHash;
  uint32_t checksum;
  bool integrityMismatch;
};

FontByteSummary summarizeFontBytes(const uint8_t* bytes, const size_t size) {
  FontByteDigest digest(HalScalableFont::renderingRevision());
  digest.add(bytes, size);
  return {digest.hash(), digest.checksum(), digest.integrityMismatch()};
}
}  // namespace
void ScalableFontAccess::configure(SemaphoreHandle_t mutex) { renderMutex = mutex; }
ScalableFontAccess::ScalableFontAccess() {
  if (renderMutex && xSemaphoreGetMutexHolder(renderMutex) != xTaskGetCurrentTaskHandle()) {
    owned_ = xSemaphoreTake(renderMutex, portMAX_DELAY) == pdTRUE;
  }
}
ScalableFontAccess::~ScalableFontAccess() {
  if (owned_) xSemaphoreGive(renderMutex);
}
HalScalableFont::~HalScalableFont() {
  ScalableFontAccess access;
  if (font_.ready() && liveRuntime) liveRuntime->clearCaches();
  font_.deinit();
  streamFile_.close();
}
// Increment the adapter revision when metric/coverage conventions change.
uint32_t HalScalableFont::renderingRevision() { return AdapterRevision; }

bool HalScalableFont::openMemory(const uint8_t* bytes, size_t size) {
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  return openMemory(bytes, size, options);
}

bool HalScalableFont::openMemory(const uint8_t* bytes, size_t size,
                                 const freeink::font::FtFont::RenderOptions& options) {
  lastLoadError_ = FontLoadError::Unsupported;
  if (!bytes || truetype::inspect(size, [&](size_t offset, uint8_t* dest, size_t count) {
                  std::memcpy(dest, bytes + offset, count);
                  return true;
                }) != truetype::Result::Supported) {
    LOG_ERR("TTF", "Unsupported or malformed static outline data");
    return false;
  }
  fontDataFailure_ = false;
  const auto summary = summarizeFontBytes(bytes, size);
  integrityChecksum_ = summary.checksum;
  integrityMismatch_ = summary.integrityMismatch;
  return openSource(bytes, size, false, options, summary.contentHash);
}

bool HalScalableFont::openSource(const uint8_t* bytes, size_t size, const bool streamed,
                                 const freeink::font::FtFont::RenderOptions& options, const uint32_t contentHash,
                                 const bool temporary) {
  ScalableFontAccess access;
  if (font_.ready()) {
    LOG_ERR("TTF", "Cannot open an already loaded font");
    lastLoadError_ = FontLoadError::Read;
    return false;
  }
  if (!runtime().begin()) {
    lastLoadError_ = runtime().beginError;
    return false;
  }
  // One bounded PSRAM allocation per face; descriptors stay stable while registered.
  // Glyph pixels and FreeType scratch still share the existing fixed pools.
  sizesStorage_ = makeAlignedByteBufferNoThrow(sizeof(Size) * SizeCount, Pool);
  sizes_ = reinterpret_cast<Size*>(sizesStorage_.get());
  if (sizes_)
    for (size_t i = 0; i < SizeCount; ++i) new (sizes_ + i) Size{};
  if (!sizes_) {
    lastLoadError_ = FontLoadError::Memory;
    LOG_ERR("TTF", "Cannot allocate size descriptors");
    return false;
  }
  runtime().allocator.clearFailure();
  const bool opened =
      streamed ? font_.initStream(streamRead, this, size, 1) : font_.init(bytes, static_cast<uint32_t>(size), 1);
  const bool optionsAccepted = opened && font_.setRenderOptions(options);
  if (!optionsAccepted) {
    lastLoadError_ = streamFailureLogged_                      ? FontLoadError::Read
                     : runtime().allocator.lastFailedRequest() ? FontLoadError::Memory
                                                               : FontLoadError::Unsupported;
    const auto initFailure = font_.lastInitFailure();
    const int initError = font_.lastInitError();
    fontDataFailure_ = !streamFailureLogged_ && runtime().allocator.lastFailedRequest() == 0 &&
                       initFailure == freeink::font::FtFont::InitFailure::OpenFace && initError != 0;
    LOG_ERR("TTF",
            "Cannot initialize %s TTF: stage=%s ftError=0x%X optionsAccepted=%u streamReadFailed=%u "
            "arenaRequest=%u free=%u largest=%u",
            streamed ? "streamed" : "resident", initFailureName(font_.lastInitFailure()),
            unsigned(font_.lastInitError()), unsigned(optionsAccepted), unsigned(streamFailureLogged_),
            unsigned(runtime().allocator.lastFailedRequest()), unsigned(runtime().allocator.freeBytes()),
            unsigned(runtime().allocator.largestFreeBlock()));
    font_.deinit();
    sizesStorage_.reset();
    sizes_ = nullptr;
    return false;
  }
  renderOptions_ = options;
  streamed_ = streamed;
  // Large streamed fonts can carry multi-hundred-KiB GPOS tables. CrossPoint
  // skips these in streamed mode, keeping the shared FreeType arena for glyphs.
  if (streamed) font_.setGposByteBudget(0);
  font_.setGsubByteBudget(48 * 1024);
  static constexpr uint32_t sequences[5][3] = {
      {'f', 'f', 0}, {'f', 'i', 0}, {'f', 'l', 0}, {'f', 'f', 'i'}, {'f', 'f', 'l'}};
  for (unsigned i = 0; i < 5; ++i) {
    ligatureGlyphs_[i] = font_.glyphId(0xfb00 + i);
    if (ligatureGlyphs_[i]) continue;
    const unsigned length = sequences[i][2] ? 3 : 2;
    ligatureGlyphs_[i] = font_.ligatureGlyphId(sequences[i], length);
  }
  font_.releaseLigatureTable();
  cacheId_ = runtime().nextCacheId++;
  if (!cacheId_) cacheId_ = runtime().nextCacheId++;
  // Temporary dictionary layouts never enter the EPUB cache. A fresh identity
  // per open prevents stale metrics without scanning every byte of the file.
  contentHash_ = temporary ? (cacheId_ ^ 0xD1C710A5u) : contentHash;
  hash_ = contentHash_;
  const auto hashOption = [this](const uint32_t value) { hash_ = (hash_ ^ value) * 16777619u; };
  hashOption(static_cast<uint8_t>(options.hinting));
  hashOption(options.monochrome ? 1u : 0u);
  hashOption(options.interpreterVersion);
  hashOption(static_cast<uint32_t>(options.embolden26_6));
  hashOption(static_cast<uint32_t>(options.slant16_16));
  hashOption(options.stemDarkening ? 1u : 0u);
  // Streamed faces skip GPOS: their layout identity must reflect that.
  hashOption(streamed_ ? 1u : 0u);
  lastLoadError_ = FontLoadError::None;
  return true;
}
bool HalScalableFont::setRenderOptions(const freeink::font::FtFont::RenderOptions& options) {
  ScalableFontAccess access;
  if (!font_.ready() || !font_.setRenderOptions(options)) return false;
  renderOptions_ = options;
  runtime().clearCaches();
  for (size_t i = 0; i < SizeCount; ++i) {
    sizes_[i].~Size();
    new (sizes_ + i) Size{};
  }
  rasterFailureLogged_ = false;
  metricFailureLogged_ = false;
  streamFailureLogged_ = false;
  fontDataFailure_ = false;
  hash_ = contentHash_;
  const auto hashOption = [this](const uint32_t value) { hash_ = (hash_ ^ value) * 16777619u; };
  hashOption(static_cast<uint8_t>(options.hinting));
  hashOption(options.monochrome ? 1u : 0u);
  hashOption(options.interpreterVersion);
  hashOption(static_cast<uint32_t>(options.embolden26_6));
  hashOption(static_cast<uint32_t>(options.slant16_16));
  hashOption(options.stemDarkening ? 1u : 0u);
  // Streamed faces skip GPOS: their layout identity must reflect that.
  hashOption(streamed_ ? 1u : 0u);
  return true;
}
bool HalScalableFont::fileSize(const char* path, size_t& size, size_t limit, FontLoadError* error) {
  // open() preserves wrapper-allocation failures; openFileForRead() only returns a bool.
  if (error) *error = FontLoadError::Read;
  HalFile file = Storage.open(path);
  if (!file) {
    if (error && file.allocationFailed()) *error = FontLoadError::Memory;
    LOG_ERR("TTF", "Cannot read font size: %s", path);
    file.close();
    return false;
  }
  size = file.size();
  file.close();
  const bool valid = validFileSize(path, size, limit);
  if (error) *error = valid ? FontLoadError::None : size > limit ? FontLoadError::TooLarge : FontLoadError::Unsupported;
  return valid;
}
bool HalScalableFont::prepareFamily(size_t bytes, size_t faces) {
  ScalableFontAccess access;
  if (!faces || faces > 4 || bytes > MaxFamilyBytes) {
    LOG_ERR("TTF", "TTF family data budget exceeded (%u bytes; limit=%u; faces=%u)", unsigned(bytes),
            unsigned(MaxFamilyBytes), unsigned(faces));
    return false;
  }
  if (!runtime().begin()) return false;
  const auto heap = byteHeapSnapshot(Pool);
  const size_t required = faces * (sizeof(Size) * SizeCount + StreamBufferBytes) + ReaderReserveBytes;
  if (heap.free < required) {
    LOG_ERR("TTF", "Insufficient memory for TTF family (%u file bytes; pool free=%u largest=%u; required=%u)",
            unsigned(bytes), unsigned(heap.free), unsigned(heap.largest), unsigned(required));
    return false;
  }
  return true;
}
bool HalScalableFont::openFile(const char* path, size_t remainingBytes) {
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  return openFile(path, remainingBytes, options);
}
bool HalScalableFont::openFile(const char* path, size_t remainingBytes,
                               const freeink::font::FtFont::RenderOptions& options, const FileMode mode,
                               const size_t pendingFaces) {
  ScalableFontAccess access;
  streamPrefix_.reset();
  streamPrefixSize_ = 0;
  fontDataFailure_ = false;
  integrityMismatch_ = false;
  integrityChecksum_ = 0;
  lastLoadError_ = FontLoadError::Read;
  if (!runtime().begin()) {
    lastLoadError_ = runtime().beginError;
    return false;
  }
  HalFile file = Storage.open(path);
  if (!file) {
    if (file.allocationFailed()) lastLoadError_ = FontLoadError::Memory;
    LOG_ERR("TTF", "Cannot open font: %s", path);
    file.close();
    return false;
  }
  const size_t size = file.size();
  if (!validFileSize(path, size, mode == FileMode::Filename ? MaxFilenameFileBytes : MaxFileBytes)) {
    lastLoadError_ = size > (mode == FileMode::Filename ? MaxFilenameFileBytes : MaxFileBytes)
                         ? FontLoadError::TooLarge
                         : FontLoadError::Unsupported;
    file.close();
    return false;
  }
  const auto format = inspectFormat(file, size);
  if (format != truetype::Result::Supported || !file.seekSet(0)) {
    lastLoadError_ = format == truetype::Result::Unsupported ? FontLoadError::Unsupported : FontLoadError::Read;
    LOG_ERR("TTF", "Unsupported or malformed static outline font: %s", path);
    file.close();
    return false;
  }
  std::strncpy(streamPath_, path, sizeof(streamPath_) - 1);
  streamPath_[sizeof(streamPath_) - 1] = '\0';
  if (size > remainingBytes) {
    lastLoadError_ = FontLoadError::TooLarge;
    LOG_ERR("TTF", "TTF family data budget exceeded: %s (%u bytes; remaining=%u)", path, unsigned(size),
            unsigned(remainingBytes));
    file.close();
    return false;
  }

  const auto heap = byteHeapSnapshot(Pool);
  // Keep descriptors and stream buffers available for the remaining styles.
  // Prefer resident regular text even when the complete family cannot fit.
  if (pendingFaces > 3) {
    lastLoadError_ = FontLoadError::Unsupported;
    LOG_ERR("TTF", "Invalid pending face count: %u", unsigned(pendingFaces));
    file.close();
    return false;
  }
  const size_t reserve = ReaderReserveBytes + (pendingFaces + 1) * (sizeof(Size) * SizeCount + StreamBufferBytes);
  if (heap.free < reserve) {
    lastLoadError_ = FontLoadError::Memory;
    LOG_ERR("TTF", "Insufficient memory for TTF face: %s (free=%u required=%u)", path, unsigned(heap.free),
            unsigned(reserve));
    file.close();
    return false;
  }
  // A small face is cheaper to read sequentially once than through hundreds of
  // tiny glyph reads. Cap temporary residency at 256 KiB per style (1 MiB per
  // family); large dictionary fonts should not scan megabytes before opening.
  constexpr size_t TemporaryResidentLimit = 256 * 1024;
  const bool allowResident = mode == FileMode::Auto || ((mode == FileMode::Temporary || mode == FileMode::Filename) &&
                                                        size <= TemporaryResidentLimit);
  const bool canReside = allowResident && size <= heap.free - reserve && heap.largest >= size;
  if (canReside) {
    auto bytes = makeAlignedByteBufferNoThrow(size, Pool);
    if (bytes) {
      const int got = file.read(bytes.get(), size);
      if (got < 0 || size_t(got) != size) {
        LOG_ERR("TTF", "Short font read: %s (%d/%u bytes)", path, got, unsigned(size));
        file.close();
        return false;
      }
      file.close();
      const bool opened = mode == FileMode::Temporary ? openSource(bytes.get(), size, false, options, 0, true)
                                                      : openMemory(bytes.get(), size, options);
      if (!opened) {
        if (integrityMismatch_)
          LOG_INF("TTF", "Font integrity warning: %s checksum=%08X expected=%08X", path, unsigned(integrityChecksum_),
                  unsigned(SfntChecksumMagic));
        LOG_ERR("TTF", "Cannot initialize resident font: %s", path);
        return false;
      }
      if (integrityMismatch_)
        LOG_INF("TTF", "Font integrity warning: %s checksum=%08X expected=%08X", path, unsigned(integrityChecksum_),
                unsigned(SfntChecksumMagic));
      bytes_ = std::move(bytes);
      fileBytes_ = size;
      LOG_DBG("TTF", "%s font: %s (%u bytes; identityHash=%08X glyphT=%u)",
              mode == FileMode::Temporary ? "Temporary resident" : "Resident", path, unsigned(size),
              unsigned(contentHash_), unsigned(font_.glyphId('T')));
      return true;
    }
  }

  // Keep a single SD handle per face. Reader loads hash the full file to detect
  // replacement without keeping its bytes in PSRAM. Temporary dictionary loads
  // need no persistent layout identity and skip that scan.
  // Read in eight-sector batches: the old 256-byte stack buffer made a typical
  // four-face family perform tens of thousands of locked SD reads at startup.
  auto chunk = makeAlignedByteBufferNoThrow(StreamBufferBytes, Pool);
  if (!chunk) {
    lastLoadError_ = FontLoadError::Memory;
    LOG_ERR("TTF", "Cannot allocate %u-byte streamed font buffer: %s", unsigned(StreamBufferBytes), path);
    file.close();
    return false;
  }
  // Reader fonts already scan the whole file for their content identity. Keep
  // its first megabyte while scanning when PSRAM can still retain the reader
  // reserve and pending style descriptors. Common sfnt lookup tables live in
  // this prefix, so later glyph faults avoid repeated SD seeks. The cache is
  // optional and never uses the C3/internal-RAM path.
  if (mode != FileMode::Temporary) {
    constexpr size_t PrefixLimit = 1024 * 1024;
    constexpr size_t ExtraHeadroom = 256 * 1024;
    const size_t prefix = std::min(size, PrefixLimit);
    const auto available = byteHeapSnapshot(Pool);
    if (available.free > reserve + ExtraHeadroom && prefix <= available.free - reserve - ExtraHeadroom &&
        available.largest > ExtraHeadroom && prefix <= available.largest - ExtraHeadroom) {
      streamPrefix_ = makeAlignedByteBufferNoThrow(prefix, Pool);
      if (streamPrefix_) streamPrefixSize_ = prefix;
    }
  }
  uint32_t contentHash = 0;
  if (mode != FileMode::Temporary) {
    FontByteDigest digest(renderingRevision());
    if (!file.seekSet(0)) {
      LOG_ERR("TTF", "Cannot rewind streamed font: %s", path);
      file.close();
      return false;
    }
#ifndef SIMULATOR
    // Large CJK files can take seconds over SPI. Let idle tasks run and feed
    // the subscribed caller while hashing, as the SDK's SD stream path does.
    const bool watchdogWatchesThisTask = esp_task_wdt_status(nullptr) == ESP_OK;
    uint32_t lastYieldMs = millis();
#endif
    for (size_t offset = 0; offset < size;) {
      const size_t count = std::min(StreamBufferBytes, size - offset);
      const int got = file.read(chunk.get(), count);
      if (got < 0 || size_t(got) != count) {
        LOG_ERR("TTF", "Short streamed font read: %s at %u (%d/%u bytes)", path, unsigned(offset), got,
                unsigned(count));
        file.close();
        return false;
      }
      digest.add(chunk.get(), count);
      if (offset < streamPrefixSize_)
        std::memcpy(streamPrefix_.get() + offset, chunk.get(), std::min(count, streamPrefixSize_ - offset));
      offset += count;
#ifndef SIMULATOR
      if (millis() - lastYieldMs >= 100) {
        if (watchdogWatchesThisTask) esp_task_wdt_reset();
        vTaskDelay(1);
        lastYieldMs = millis();
      }
#endif
    }
    contentHash = digest.hash();
    integrityChecksum_ = digest.checksum();
    integrityMismatch_ = digest.integrityMismatch();
    if (integrityMismatch_)
      LOG_INF("TTF", "Font integrity warning: %s checksum=%08X expected=%08X", path, unsigned(integrityChecksum_),
              unsigned(SfntChecksumMagic));
    if (!file.seekSet(0)) {
      LOG_ERR("TTF", "Cannot rewind streamed font: %s", path);
      file.close();
      return false;
    }
  }
  streamFile_ = std::move(file);
  std::strncpy(streamPath_, path, sizeof(streamPath_) - 1);
  streamPath_[sizeof(streamPath_) - 1] = '\0';
  fileBytes_ = size;
  streamFailureLogged_ = false;
  streamBuffer_ = std::move(chunk);
  for (auto& window : streamWindows_) window = {};
  nextStreamWindow_ = 0;
  if (!openSource(nullptr, size, true, options, contentHash, mode == FileMode::Temporary)) {
    LOG_ERR("TTF", "Cannot initialize streamed font: %s", path);
    streamFile_.close();
    streamPrefix_.reset();
    streamPrefixSize_ = 0;
    fileBytes_ = 0;
    return false;
  }
  if (streamPrefixSize_)
    LOG_DBG("TTF", "Cached %u KB font prefix in %s: %s", unsigned(streamPrefixSize_ / 1024), memoryPoolName(Pool),
            path);
  LOG_DBG("TTF", "%s font: %s (%u bytes; identityHash=%08X glyphT=%u; pool free=%u largest=%u)",
          mode == FileMode::Temporary ? "Temporary streamed" : "Streaming", path, unsigned(size),
          unsigned(contentHash_), unsigned(font_.glyphId('T')), unsigned(heap.free), unsigned(heap.largest));
  return true;
}

unsigned long HalScalableFont::streamRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count) {
  auto& self = *static_cast<HalScalableFont*>(ctx);
  if (!count) return 0;
  int got = -1;
  if (offset <= self.fileBytes_ && count <= self.fileBytes_ - offset) {
    if (offset < self.streamPrefixSize_) {
      const size_t cached = std::min<size_t>(count, self.streamPrefixSize_ - offset);
      std::memcpy(buffer, self.streamPrefix_.get() + offset, cached);
      if (cached == count) return count;
      return cached + streamRead(ctx, offset + cached, buffer + cached, count - cached);
    }
    for (size_t i = 0; i < StreamWindowCount; ++i) {
      const auto& window = self.streamWindows_[i];
      if (offset >= window.offset && offset - window.offset <= window.size &&
          count <= window.size - (offset - window.offset)) {
        std::memcpy(buffer, self.streamBuffer_.get() + i * StreamWindowBytes + offset - window.offset, count);
        return count;
      }
    }
    // FreeType alternates between outline and metric tables. Separate small
    // windows retain both instead of rereading a single 4 KiB window on each
    // switch. Larger requests go directly to the caller's buffer.
    constexpr size_t SectorBytes = 512;
    const size_t alignedOffset = offset - (offset % SectorBytes);
    const size_t leadingBytes = offset - alignedOffset;
    if (count <= StreamWindowBytes - leadingBytes && self.streamBuffer_) {
      const size_t slot = self.nextStreamWindow_++ % StreamWindowCount;
      auto& window = self.streamWindows_[slot];
      window.size = 0;
      auto* bytes = self.streamBuffer_.get() + slot * StreamWindowBytes;
      const size_t buffered = std::min(StreamWindowBytes, self.fileBytes_ - alignedOffset);
      const int bufferedGot = self.streamFile_.seekSet(alignedOffset) ? self.streamFile_.read(bytes, buffered) : -1;
      if (bufferedGot >= 0 && size_t(bufferedGot) == buffered) {
        window = {alignedOffset, buffered};
        std::memcpy(buffer, bytes + leadingBytes, count);
        return count;
      }
      // A failed read-ahead must not reject a smaller valid request. Retrying
      // the exact read also lets a transient SD failure recover.
    }
    if (self.streamFile_.seekSet(offset)) got = self.streamFile_.read(buffer, count);
  }
  if (got < 0 || static_cast<unsigned long>(got) != count) {
    if (!self.streamFailureLogged_) {
      self.streamFailureLogged_ = true;
      LOG_ERR("TTF", "Stream read failed: %s offset=%u count=%u got=%d", self.streamPath_, unsigned(offset),
              unsigned(count), got);
    }
    return got < 0 ? 0 : static_cast<unsigned long>(got);
  }
  return count;
}
bool HalScalableFont::copyFamilyName(char* name, const size_t capacity) {
  ScalableFontAccess access;
  if (!name || capacity == 0) return false;
  name[0] = '\0';
  if (!font_.ready() || (!streamed_ && !bytes_)) return false;
  freeink::font::FtFont::FaceInfo info;
  const auto result = streamed_
                          ? freeink::font::FtFont::inspectStream(streamRead, this, fileBytes_, info, name, capacity)
                          : freeink::font::FtFont::inspectMemory(bytes_.get(), fileBytes_, info, name, capacity);
  if (result != freeink::font::FtFont::InspectResult::Ok || !name[0]) {
    LOG_ERR("TTF", "Cannot read loaded font family name");
    name[0] = '\0';
    return false;
  }
  return true;
}

bool HalScalableFont::inspectFile(const char* path, Info& info, bool* unavailable, size_t limit, FontLoadError* error) {
  const auto failed = [&](FontLoadError reason) {
    if (error) *error = reason;
    if (unavailable) *unavailable = reason == FontLoadError::Memory || reason == FontLoadError::Read;
    return false;
  };
  if (unavailable) *unavailable = false;
  if (error) *error = FontLoadError::None;
  HalFile file = Storage.open(path);
  if (!file) {
    const auto reason = file.allocationFailed() ? FontLoadError::Memory : FontLoadError::Read;
    LOG_ERR("TTF", "Cannot inspect font: %s", path);
    file.close();
    return failed(reason);
  }
  const size_t size = file.size();
  if (!validFileSize(path, size, limit)) {
    file.close();
    return failed(size > limit ? FontLoadError::TooLarge : FontLoadError::Unsupported);
  }
  info = Info{};
  const auto format = inspectFormat(file, size);
  if (format != truetype::Result::Supported) {
    uint8_t signature[4] = {};
    const bool signatureRead = file.seekSet(0) && file.read(signature, sizeof(signature)) == sizeof(signature);
    LOG_ERR("TTF",
            "Unsupported static outline font: %s (preflight=%u cff=%u size=%u signature=%02X%02X%02X%02X read=%u)",
            path, unsigned(format), unsigned(truetype::kCffEnabled), unsigned(size), unsigned(signature[0]),
            unsigned(signature[1]), unsigned(signature[2]), unsigned(signature[3]), unsigned(signatureRead));
    file.close();
    return failed(format == truetype::Result::Unavailable ? FontLoadError::Read : FontLoadError::Unsupported);
  }
  if (!runtime().begin()) {
    file.close();
    return failed(runtime().beginError);
  }
  freeink::font::FtFont::FaceInfo details;
  struct Source {
    HalFile* file;
    bool failed = false;
  } source{&file};
  const auto read = [](void* context, unsigned long offset, unsigned char* bytes, unsigned long count) {
    auto& source = *static_cast<Source*>(context);
    const auto got =
        source.file->seekSet(offset) ? static_cast<unsigned long>(std::max(0, source.file->read(bytes, count))) : 0ul;
    if (got != count) source.failed = true;
    return got;
  };
  runtime().allocator.clearFailure();
  const auto result =
      freeink::font::FtFont::inspectStream(read, &source, size, details, info.family, sizeof(info.family));
  file.close();
  if (result != freeink::font::FtFont::InspectResult::Ok) {
    LOG_ERR("TTF", "Unsupported or unavailable font: %s", path);
    return failed(source.failed                                                 ? FontLoadError::Read
                  : runtime().allocator.lastFailedRequest()                     ? FontLoadError::Memory
                  : result == freeink::font::FtFont::InspectResult::Unsupported ? FontLoadError::Unsupported
                                                                                : FontLoadError::Read);
  }
  if (!details.familyLength || details.familyLength >= sizeof(info.family)) {
    LOG_ERR("TTF", "TTF family name exceeds %u bytes: %s", unsigned(sizeof(info.family) - 1), path);
    return failed(FontLoadError::Unsupported);
  }
  info.style = (details.weight >= 600 ? 1 : 0) | (details.italic ? 2 : 0);
  return info.family[0] != '\0';
}
const EpdFont* HalScalableFont::atSize(uint8_t points) {
  ScalableFontAccess access;
  if (!font_.ready() || points < MinPointSize || points > MaxPointSize) return nullptr;
  auto& size = sizes_[points - MinPointSize];
  if (!size.owner) {
    size.points = points;
    auto& d = size.data;
    d.is2Bit = true;
    d.sizeFamily = this;
    d.pointSize = points;
    d.glyphMissCtx = &size;
    d.dynamicGlyphHandler = glyph;
    d.bitmapHandler = bitmap;
    d.coverageHandler = covers;
    d.kerningHandler = kerning;
    d.ligatureHandler = ligature;
    freeink::font::FtFont::LineMetrics metrics;
    if (!font_.lineMetrics26_6(scalableFontPixelSize26_6(points), metrics)) {
      LOG_ERR("TTF", "Line metrics failed at %u pt: %s (size=%u)", unsigned(points), streamPath_,
              unsigned(scalableFontPixelSize26_6(points)));
      return nullptr;
    }
    d.ascender = ceil26_6(metrics.ascender26_6);
    d.descender = floor26_6(metrics.descender26_6);
    d.advanceY = uint8_t(std::clamp(ceil26_6(metrics.height26_6), 1, 255));
    size.owner = this;
  }
  return &size.font;
}
bool HalScalableFont::hasCodepoint(const uint32_t cp) {
  ScalableFontAccess access;
  return font_.ready() && font_.hasGlyph(cp);
}
bool HalScalableFont::probeGlyph(const uint32_t cp, const uint8_t points) {
  if (!atSize(points)) return false;
  auto& size = sizes_[points - MinPointSize];
  const EpdGlyph* result = glyph(&size, cp);
  if (!result) return false;
  return !result->width || !result->height || bitmap(&size, result);
}
const EpdGlyph* HalScalableFont::glyph(void* ctx, uint32_t cp) {
  ScalableFontAccess access;
  auto& s = *static_cast<Size*>(ctx);
  s.owner->fontDataFailure_ = false;
  const auto ligatureGlyph = cp >= 0xfb00 && cp <= 0xfb04 ? s.owner->ligatureGlyphs_[cp - 0xfb00] : 0;
  auto& g = s.glyphs[s.cursor++ % 32];
  if (!runtime().glyphMetrics(s.owner->font_, s.owner->cacheId_, ligatureGlyph ? GlyphIdMarker | ligatureGlyph : cp,
                              s.points, g)) {
    const auto failure = s.owner->font_.lastGlyphFailure();
    const int error = s.owner->font_.lastGlyphError();
    s.owner->fontDataFailure_ = !s.owner->streamFailureLogged_ && runtime().allocator.lastFailedRequest() == 0 &&
                                ((failure == freeink::font::FtFont::GlyphFailure::Load ||
                                  failure == freeink::font::FtFont::GlyphFailure::Embolden ||
                                  failure == freeink::font::FtFont::GlyphFailure::Render) &&
                                 error != 0);
    if (!s.owner->metricFailureLogged_) {
      s.owner->metricFailureLogged_ = true;
      const auto& arena = runtime().allocator;
      LOG_ERR("TTF",
              "Metrics failed U+%04X at %u pt: %s stage=%s ftError=0x%X size=%u glyph=%u bounds=%ux%u "
              "streamReadFailed=%u hint=%u mono=%u embolden26_6=%d slant=%d dark=%u arenaRequest=%u free=%u "
              "largest=%u",
              unsigned(cp), unsigned(s.points), s.owner->streamPath_,
              glyphFailureName(s.owner->font_.lastGlyphFailure()), unsigned(s.owner->font_.lastGlyphError()),
              unsigned(scalableFontPixelSize26_6(s.points)), unsigned(s.owner->font_.glyphId(cp)),
              unsigned(runtime().failedMetricWidth), unsigned(runtime().failedMetricHeight),
              unsigned(s.owner->streamFailureLogged_), unsigned(s.owner->renderOptions_.hinting),
              unsigned(s.owner->renderOptions_.monochrome), int(s.owner->renderOptions_.embolden26_6),
              int(s.owner->renderOptions_.slant16_16), unsigned(s.owner->renderOptions_.stemDarkening),
              unsigned(arena.lastFailedRequest()), unsigned(arena.freeBytes()), unsigned(arena.largestFreeBlock()));
    }
    return nullptr;
  }
  return &g;
}
const uint8_t* HalScalableFont::bitmap(void* ctx, const EpdGlyph* g) {
  ScalableFontAccess access;
  auto& s = *static_cast<Size*>(ctx);
  s.owner->fontDataFailure_ = false;
  const auto* pixels =
      runtime().packedBitmap(s.owner->font_, s.owner->cacheId_, g->dataOffset, s.points, g->width, g->height);
  if (!pixels && g->width && g->height) {
    const auto failure = s.owner->font_.lastGlyphFailure();
    const int error = s.owner->font_.lastGlyphError();
    s.owner->fontDataFailure_ = !s.owner->streamFailureLogged_ && runtime().allocator.lastFailedRequest() == 0 &&
                                ((failure == freeink::font::FtFont::GlyphFailure::Load ||
                                  failure == freeink::font::FtFont::GlyphFailure::Embolden ||
                                  failure == freeink::font::FtFont::GlyphFailure::Render) &&
                                 error != 0);
  }
  if (!pixels && g->width && g->height && !s.owner->rasterFailureLogged_) {
    s.owner->rasterFailureLogged_ = true;
    LOG_ERR("TTF",
            "Bitmap failed glyph=%u at %u pt: %s stage=%s ftError=0x%X expected=%ux%u actual=%ux%u "
            "arenaRequest=%u",
            unsigned(g->dataOffset), unsigned(s.points), s.owner->streamPath_,
            glyphFailureName(s.owner->font_.lastGlyphFailure()), unsigned(s.owner->font_.lastGlyphError()),
            unsigned(g->width), unsigned(g->height), unsigned(runtime().lastBitmapWidth),
            unsigned(runtime().lastBitmapHeight), unsigned(runtime().allocator.lastFailedRequest()));
  }
  return pixels;
}
bool HalScalableFont::covers(void* ctx, uint32_t cp) {
  ScalableFontAccess access;
  auto& s = *static_cast<Size*>(ctx);
  if (cp >= 0xfb00 && cp <= 0xfb04 && s.owner->ligatureGlyphs_[cp - 0xfb00]) return true;
  return s.owner->font_.hasGlyph(cp);
}
int8_t HalScalableFont::kerning(void* ctx, uint32_t a, uint32_t b) {
  ScalableFontAccess access;
  auto& s = *static_cast<Size*>(ctx);
  const auto glyph = [&](uint32_t cp) {
    const auto ligature = cp >= 0xfb00 && cp <= 0xfb04 ? s.owner->ligatureGlyphs_[cp - 0xfb00] : 0;
    return ligature ? ligature : s.owner->font_.glyphId(cp);
  };
  return runtime().kerning(s.owner->font_, s.owner->cacheId_, glyph(a), glyph(b), s.points);
}
uint32_t HalScalableFont::ligature(void* ctx, uint32_t a, uint32_t b) {
  ScalableFontAccess access;
  auto& s = *static_cast<Size*>(ctx);
  uint32_t codepoint = 0;
  if (a == 'f') {
    if (b == 'f')
      codepoint = 0xfb00;
    else if (b == 'i')
      codepoint = 0xfb01;
    else if (b == 'l')
      codepoint = 0xfb02;
  } else if (a == 0xfb00) {
    if (b == 'i')
      codepoint = 0xfb03;
    else if (b == 'l')
      codepoint = 0xfb04;
  }
  return codepoint && s.owner->ligatureGlyphs_[codepoint - 0xfb00] ? codepoint : 0;
}
#endif
