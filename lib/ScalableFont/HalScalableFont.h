#pragma once
#if CROSSINK_SCALABLE_FONTS
#include <EpdFontFamily.h>
#include <FtFont.h>
#include <HalStorage.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>

#include "FontLoadError.h"
#include "ScalableFontSizing.h"

// Borrows the app's render mutex when already held; otherwise owns it for
// this scope. A whole draw retains the outer RenderLock and borrowed pixels.
class ScalableFontAccess {
 public:
  static void configure(SemaphoreHandle_t mutex);
  ScalableFontAccess();
  ~ScalableFontAccess();
  ScalableFontAccess(const ScalableFontAccess&) = delete;
  ScalableFontAccess& operator=(const ScalableFontAccess&) = delete;

 private:
  bool owned_ = false;
};

// The HAL adapter owns font files and translates SDK coverage into CrossInk's
// existing 2-bit and fractional-metric conventions. Call under RenderLock.
class HalScalableFont {
 public:
  static constexpr size_t MaxFileBytes = 2 * 1024 * 1024;
  static constexpr size_t MaxFilenameFileBytes = 32 * 1024 * 1024;
  static constexpr size_t MaxFamilyBytes = 6 * 1024 * 1024;
  static constexpr size_t ReaderReserveBytes = 1024 * 1024;
  struct Info {
    char family[64] = {};
    uint8_t style = 0;
    bool variable = false;
  };
  // Temporary uses a per-open identity and streams large faces; never persist
  // its layouts. Auto and Stream retain content-based reader cache identities.
  enum class FileMode { Auto, Stream, Temporary, Filename };
  HalScalableFont() = default;
  ~HalScalableFont();
  HalScalableFont(const HalScalableFont&) = delete;
  HalScalableFont& operator=(const HalScalableFont&) = delete;
  bool openMemory(const uint8_t* bytes, size_t size);
  bool openMemory(const uint8_t* bytes, size_t size, const freeink::font::FtFont::RenderOptions& options);
  bool openFile(const char* path, size_t remainingBytes = MaxFileBytes);
  bool openFile(const char* path, size_t remainingBytes, const freeink::font::FtFont::RenderOptions& options,
                FileMode mode = FileMode::Auto, size_t pendingFaces = 0);
  // The caller must unregister every EpdFont returned by atSize() before
  // changing options because their cached metrics are rebuilt in place.
  bool setRenderOptions(const freeink::font::FtFont::RenderOptions& options);
  static bool fileSize(const char* path, size_t& size, size_t limit = MaxFileBytes, FontLoadError* error = nullptr);
  static bool prepareFamily(size_t bytes, size_t faces);
  size_t fileBytes() const { return fileBytes_; }
  // OpenType's whole-file checksum is advisory: some usable fonts ship with
  // stale checksums, so callers must combine this with a real load failure.
  bool integrityMismatch() const { return integrityMismatch_; }
  bool lastFailureLooksLikeFontData() const { return fontDataFailure_; }
  FontLoadError lastLoadError() const { return lastLoadError_; }
  bool hasCodepoint(uint32_t cp);
  bool probeGlyph(uint32_t cp, uint8_t points);
  static bool inspectFile(const char* path, Info& info, bool* unavailable = nullptr, size_t limit = MaxFileBytes,
                          FontLoadError* error = nullptr);
  static uint32_t renderingRevision();
  uint32_t fingerprint() const { return hash_; }
  // Reads metadata through the already-owned source; never reopens an SD reader.
  bool copyFamilyName(char* name, size_t capacity);
  const EpdFont* atSize(uint8_t points);

 private:
  struct Size {
    HalScalableFont* owner = nullptr;
    uint8_t points = 0;
    EpdFontData data{};
    EpdFont font{&data};
    // Returned metrics survive subsequent lookups in the same draw operation.
    EpdGlyph glyphs[32]{};
    uint8_t cursor = 0;
  };
  static const EpdGlyph* glyph(void* ctx, uint32_t cp);
  static const uint8_t* bitmap(void* ctx, const EpdGlyph* glyph);
  static bool covers(void* ctx, uint32_t cp);
  static int8_t kerning(void* ctx, uint32_t left, uint32_t right);
  static uint32_t ligature(void* ctx, uint32_t left, uint32_t right);
  static unsigned long streamRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
  static constexpr size_t StreamWindowBytes = 1024;
  static constexpr size_t StreamWindowCount = 4;
  static constexpr size_t StreamBufferBytes = StreamWindowBytes * StreamWindowCount;
  bool openSource(const uint8_t* bytes, size_t size, bool streamed, const freeink::font::FtFont::RenderOptions& options,
                  uint32_t contentHash, bool temporary = false);
  static constexpr uint8_t MinPointSize = ScalableContentMinPointSize;
  static constexpr uint8_t MaxPointSize = ScalableContentMaxPointSize;
  static constexpr size_t SizeCount = MaxPointSize - MinPointSize + 1;
  uint32_t cacheId_ = 0;
  bool metricFailureLogged_ = false;
  bool rasterFailureLogged_ = false;
  bool streamFailureLogged_ = false;
  bool integrityMismatch_ = false;
  bool fontDataFailure_ = false;
  FontLoadError lastLoadError_ = FontLoadError::None;
  uint32_t integrityChecksum_ = 0;
  uint32_t ligatureGlyphs_[5] = {};
  uint32_t hash_ = 0;
  uint32_t contentHash_ = 0;
  freeink::font::FtFont::RenderOptions renderOptions_{};
  size_t fileBytes_ = 0;
  HeapByteBuffer bytes_;
  HeapByteBuffer sizesStorage_;
  Size* sizes_ = nullptr;
  char streamPath_[128] = {};
  HalFile streamFile_;
  HeapByteBuffer streamBuffer_;
  HeapByteBuffer streamPrefix_;
  size_t streamPrefixSize_ = 0;
  struct StreamWindow {
    size_t offset = 0;
    size_t size = 0;
  };
  StreamWindow streamWindows_[StreamWindowCount]{};
  size_t nextStreamWindow_ = 0;
  bool streamed_ = false;
  // Declared last so it releases borrowed bytes/the stream before their owners.
  freeink::font::FtFont font_;
};
#endif
