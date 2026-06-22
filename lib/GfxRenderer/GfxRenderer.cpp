#include "GfxRenderer.h"

#include <BidiUtils.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <freertos/task.h>

#include <algorithm>

#include "FontCacheManager.h"

namespace {

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

GfxRenderer::BitmapScratchLock::BitmapScratchLock(const GfxRenderer& renderer) : renderer_(renderer) {
  if (renderer_.bitmapScratchMutex_ == nullptr) {
    LOG_ERR("GFX", "!! Bitmap scratch mutex is not initialized");
    assert(false);
    return;
  }

  const auto takeResult = xSemaphoreTake(renderer_.bitmapScratchMutex_, portMAX_DELAY);
  if (takeResult != pdTRUE) {
    LOG_ERR("GFX", "!! Failed to acquire bitmap scratch mutex");
    assert(false);
    return;
  }

  locked_ = true;
}

GfxRenderer::BitmapScratchLock::~BitmapScratchLock() {
  if (!locked_) return;
  xSemaphoreGive(renderer_.bitmapScratchMutex_);
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    int missed = it->second->buildAdvanceTable(words, includeHyphen, styleMask);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

bool GfxRenderer::releaseSdCardFontForLowMemory(int fontId) const {
  auto it = sdCardFonts_.find(fontId);
  if (it == sdCardFonts_.end()) return false;

  it->second->releaseForLowMemory();
  return true;
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::freeBitmapScratchBuffers() {
  free(bitmapScratchOutputRow_);
  bitmapScratchOutputRow_ = nullptr;
  bitmapScratchOutputRowSize_ = 0;

  free(bitmapScratchRowBytes_);
  bitmapScratchRowBytes_ = nullptr;
  bitmapScratchRowBytesSize_ = 0;
}

bool GfxRenderer::bitmapScratchLockHeldByCurrentTask() const {
  if (bitmapScratchMutex_ == nullptr) return false;
  return xSemaphoreGetMutexHolder(bitmapScratchMutex_) == xTaskGetCurrentTaskHandle();
}

bool GfxRenderer::ensureBitmapScratchBuffers(const size_t outputRowSize, const size_t rowBytesSize) const {
  if (!bitmapScratchLockHeldByCurrentTask()) {
    LOG_ERR("GFX", "!! Bitmap scratch buffers used without holding scratch mutex");
    assert(false);
    return false;
  }

  if (outputRowSize > bitmapScratchOutputRowSize_) {
    auto* grownOutput = static_cast<uint8_t*>(realloc(bitmapScratchOutputRow_, outputRowSize));
    if (!grownOutput) {
      LOG_ERR("GFX", "!! Failed to grow BMP output row scratch buffer to %zu bytes", outputRowSize);
      return false;
    }
    bitmapScratchOutputRow_ = grownOutput;
    bitmapScratchOutputRowSize_ = outputRowSize;
  }

  if (rowBytesSize > bitmapScratchRowBytesSize_) {
    auto* grownRowBytes = static_cast<uint8_t*>(realloc(bitmapScratchRowBytes_, rowBytesSize));
    if (!grownRowBytes) {
      LOG_ERR("GFX", "!! Failed to grow BMP row-bytes scratch buffer to %zu bytes", rowBytesSize);
      return false;
    }
    bitmapScratchRowBytes_ = grownRowBytes;
    bitmapScratchRowBytesSize_ = rowBytesSize;
  }

  return true;
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

enum class TextRotation { None, Rotated90CW };

struct SyntheticSolidGlyphMetrics {
  uint16_t advanceX;
  int ascender;
  int left;
  int top;
  int width;
  int height;
};

static void fillRectClipped(const GfxRenderer& renderer, int x, int y, int width, int height, const bool pixelState) {
  if (width <= 0 || height <= 0) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int x2 = x + width;
  const int y2 = y + height;
  if (x >= screenWidth || y >= screenHeight || x2 <= 0 || y2 <= 0) return;

  const int clippedX = std::max(0, x);
  const int clippedY = std::max(0, y);
  const int clippedX2 = std::min(screenWidth, x2);
  const int clippedY2 = std::min(screenHeight, y2);
  renderer.fillRect(clippedX, clippedY, clippedX2 - clippedX, clippedY2 - clippedY, pixelState);
}

static void drawPixelClipped(const GfxRenderer& renderer, const int x, const int y, const bool pixelState) {
  if (x < 0 || x >= renderer.getScreenWidth() || y < 0 || y >= renderer.getScreenHeight()) return;
  renderer.drawPixel(x, y, pixelState);
}

static void drawIconPixels(const GfxRenderer& renderer, const uint8_t bitmap[], const int x, const int y,
                           const int width, const int height, const bool pixelState) {
  if (!bitmap || width <= 0 || height <= 0) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) return;

  const int srcStride = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    const int screenY = y + row;
    if (screenY < 0 || screenY >= screenHeight) continue;

    const int srcOffset = row * srcStride;
    for (int col = 0; col < width; ++col) {
      const int screenX = x + col;
      if (screenX < 0 || screenX >= screenWidth) continue;

      const uint8_t mask = static_cast<uint8_t>(0x80 >> (col & 7));
      if ((bitmap[srcOffset + (col >> 3)] & mask) == 0) {
        renderer.drawPixel(screenX, screenY, pixelState);
      }
    }
  }
}

static SyntheticSolidGlyphMetrics getSyntheticSolidGlyphMetrics(const EpdFontFamily& font,
                                                                const EpdFontFamily::Style style, const uint32_t cp) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::solidAdvanceX(data, font.getGlyph('M', style));
  const int height = syntheticGlyph::solidHeight(data, cp);
  const int width = syntheticGlyph::solidWidth(cp, advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {
      advanceX, ascender, syntheticGlyph::solidLeft(cp, advanceX, width), syntheticGlyph::solidTop(data, cp, height),
      width,    height};
}

static SyntheticSolidGlyphMetrics getSyntheticGreekGlyphMetrics(const EpdFontFamily& font,
                                                                const EpdFontFamily::Style style, const uint32_t cp) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::greekAdvanceX(data, font.getGlyph('M', style), cp);
  const int height = syntheticGlyph::greekHeight(data, cp);
  const int width = syntheticGlyph::greekWidth(cp, advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {
      advanceX, ascender, syntheticGlyph::greekLeft(cp, advanceX, width), syntheticGlyph::greekTop(data, cp, height),
      width,    height};
}

static SyntheticSolidGlyphMetrics getSyntheticReplacementGlyphMetrics(const EpdFontFamily& font,
                                                                      const EpdFontFamily::Style style) {
  const EpdFontData* data = font.getData(style);
  const uint16_t advanceX = syntheticGlyph::replacementAdvanceX(data, font.getGlyph('M', style));
  const int height = syntheticGlyph::replacementHeight(data);
  const int width = syntheticGlyph::replacementWidth(advanceX, height);
  const int ascender = data && data->ascender > 0 ? data->ascender : height;
  return {advanceX,
          ascender,
          syntheticGlyph::replacementLeft(advanceX, width),
          syntheticGlyph::replacementTop(data, height),
          width,
          height};
}

static void fillSyntheticSolidGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                    const int cursorX, const int baselineY, const bool pixelState) {
  fillRectClipped(renderer, cursorX + metrics.left, baselineY - metrics.top, metrics.width, metrics.height, pixelState);
}

static void fillSyntheticSolidGlyphRotated90CW(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                               const int cursorX, const int cursorY, const bool pixelState) {
  fillRectClipped(renderer, cursorX + metrics.ascender - metrics.top, cursorY - metrics.left - metrics.width + 1,
                  metrics.height, metrics.width, pixelState);
}

static int syntheticStroke(const SyntheticSolidGlyphMetrics& metrics) {
  const int minDim = metrics.width < metrics.height ? metrics.width : metrics.height;
  return minDim >= 14 ? 2 : 1;
}

static bool epsilonTemplatePixel(const int srcX, const int srcY) {
  static constexpr uint8_t EPSILON_7X7[] = {
      0b0111110, 0b1100000, 0b1000000, 0b1111100, 0b1000000, 0b1100000, 0b0111110,
  };
  return (EPSILON_7X7[srcY] & (1 << (6 - srcX))) != 0;
}

static bool questionTemplatePixel(const int srcX, const int srcY) {
  static constexpr uint8_t QUESTION_7X9[] = {
      0b0011100, 0b0100010, 0b0000010, 0b0000100, 0b0001000, 0b0001000, 0b0000000, 0b0001000, 0b0001000,
  };
  return (QUESTION_7X9[srcY] & (1 << (6 - srcX))) != 0;
}

static void drawSyntheticReplacementGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                          const int cursorX, const int baselineY, const bool pixelState) {
  const int x = cursorX + metrics.left;
  const int y = baselineY - metrics.top;
  const int w = metrics.width;
  const int h = metrics.height;
  if (w <= 0 || h <= 0) return;

  const int limit = w < h ? w : h;
  for (int gy = 0; gy < h; gy++) {
    for (int gx = 0; gx < w; gx++) {
      const int diamondX = abs((2 * gx + 1) - w);
      const int diamondY = abs((2 * gy + 1) - h);
      if (diamondX + diamondY <= limit) {
        drawPixelClipped(renderer, x + gx, y + gy, pixelState);
      }
    }
  }

  if (w < 7 || h < 9) return;
  const int qW = w > 10 ? 7 : 5;
  const int qH = h > 12 ? 9 : 7;
  const int qX = x + (w - qW) / 2;
  const int qY = y + (h - qH) / 2;
  for (int gy = 0; gy < qH; gy++) {
    for (int gx = 0; gx < qW; gx++) {
      if (questionTemplatePixel(gx * 7 / qW, gy * 9 / qH)) {
        drawPixelClipped(renderer, qX + gx, qY + gy, !pixelState);
      }
    }
  }
}

static void drawSyntheticReplacementGlyphRotated90CW(const GfxRenderer& renderer,
                                                     const SyntheticSolidGlyphMetrics& metrics, const int cursorX,
                                                     const int cursorY, const bool pixelState) {
  const int baseX = cursorX + metrics.ascender - metrics.top;
  const int baseY = cursorY - metrics.left;
  const int w = metrics.width;
  const int h = metrics.height;
  if (w <= 0 || h <= 0) return;

  const int limit = w < h ? w : h;
  for (int gy = 0; gy < h; gy++) {
    for (int gx = 0; gx < w; gx++) {
      const int diamondX = abs((2 * gx + 1) - w);
      const int diamondY = abs((2 * gy + 1) - h);
      if (diamondX + diamondY <= limit) {
        drawPixelClipped(renderer, baseX + gy, baseY - gx, pixelState);
      }
    }
  }

  if (w < 7 || h < 9) return;
  const int qW = w > 10 ? 7 : 5;
  const int qH = h > 12 ? 9 : 7;
  const int qLeft = (w - qW) / 2;
  const int qTop = (h - qH) / 2;
  for (int gy = 0; gy < qH; gy++) {
    for (int gx = 0; gx < qW; gx++) {
      if (questionTemplatePixel(gx * 7 / qW, gy * 9 / qH)) {
        drawPixelClipped(renderer, baseX + qTop + gy, baseY - (qLeft + gx), !pixelState);
      }
    }
  }
}

static void drawSyntheticGreekGlyph(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                    const uint32_t cp, const int cursorX, const int baselineY, const bool pixelState) {
  const int x = cursorX + metrics.left;
  const int y = baselineY - metrics.top;
  const int w = metrics.width;
  const int h = metrics.height;
  const int s = syntheticStroke(metrics);
  if (w <= 0 || h <= 0) return;

  if (cp == syntheticGlyph::GREEK_CAPITAL_GAMMA) {
    fillRectClipped(renderer, x, y, s, h, pixelState);
    fillRectClipped(renderer, x, y, w, s, pixelState);
  } else if (cp == syntheticGlyph::GREEK_SMALL_EPSILON) {
    for (int gy = 0; gy < h; gy++) {
      const int srcY = gy * 7 / h;
      for (int gx = 0; gx < w; gx++) {
        if (epsilonTemplatePixel(gx * 7 / w, srcY)) drawPixelClipped(renderer, x + gx, y + gy, pixelState);
      }
    }
  } else if (cp == syntheticGlyph::GREEK_SMALL_OMEGA) {
    const int mid = w / 2;
    fillRectClipped(renderer, x, y + s, s, h - 2 * s, pixelState);
    fillRectClipped(renderer, x + w - s, y + s, s, h - 2 * s, pixelState);
    fillRectClipped(renderer, x + s, y + h - s, mid - s, s, pixelState);
    fillRectClipped(renderer, x + mid, y + h - s, w - mid - s, s, pixelState);
    fillRectClipped(renderer, x + mid - s / 2, y + h / 2, s, h / 2, pixelState);
  }
}

static void drawSyntheticGreekGlyphRotated90CW(const GfxRenderer& renderer, const SyntheticSolidGlyphMetrics& metrics,
                                               const uint32_t cp, const int cursorX, const int cursorY,
                                               const bool pixelState) {
  const int baseX = cursorX + metrics.ascender - metrics.top;
  const int baseY = cursorY - metrics.left;
  const int s = syntheticStroke(metrics);
  for (int gy = 0; gy < metrics.height; gy++) {
    for (int gx = 0; gx < metrics.width; gx++) {
      bool draw = false;
      if (cp == syntheticGlyph::GREEK_CAPITAL_GAMMA) {
        draw = gx < s || gy < s;
      } else if (cp == syntheticGlyph::GREEK_SMALL_EPSILON) {
        draw = epsilonTemplatePixel(gx * 7 / metrics.width, gy * 7 / metrics.height);
      } else if (cp == syntheticGlyph::GREEK_SMALL_OMEGA) {
        draw = (gx < s && gy >= s && gy < metrics.height - s) ||
               (gx >= metrics.width - s && gy >= s && gy < metrics.height - s) ||
               (gy >= metrics.height - s && gx >= s && gx < metrics.width - s) ||
               (gx >= metrics.width / 2 - s / 2 && gx < metrics.width / 2 - s / 2 + s && gy >= metrics.height / 2);
      }
      if (draw) drawPixelClipped(renderer, baseX + gy, baseY - gx, pixelState);
    }
  }
}

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph at 50% scale. Used for SUP/SUB style bits.
//
// Each destination pixel represents a 2x2 source block. Drawing when that block
// contains ink preserves thin strokes that nearest-neighbor sampling can skip.
//
// The advance width is also halved in drawText() so layout reserves exactly the right
// horizontal space for the scaled glyph.
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  // Scale the glyph bearing by the same factor so the scaled glyph sits at the correct
  // pixel offset from the (already-shifted) cursor position.
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = cursorY - glyph->top / 2;

  if (fontData->is2Bit) {
    // 2-bit packed format: 4 pixels per byte, MSB first, 2 bits per pixel.
    // raw value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        uint8_t coverage = 0;
        uint8_t maxRaw = 0;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 2];
            const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
            coverage += raw;
            if (raw > maxRaw) maxRaw = raw;
          }
        }
        if (maxRaw >= 2 || coverage >= 2) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  } else {
    // 1-bit packed format: 8 pixels per byte, MSB first.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 3];
            const uint8_t bit = 7 - (pos & 7);
            if ((byte >> bit) & 1) {
              hasInk = true;
            }
          }
        }
        if (hasInk) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  }
}

template <TextRotation rotation = TextRotation::None>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const auto glyphData = fontFamily.getGlyphData(cp, style);
  const EpdGlyph* glyph = glyphData.glyph;
  const EpdFontData* fontData = glyphData.fontData;
  if (!glyph || !fontData) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // Dedicated X3 gray LUTs now provide proper 4-level gray on both devices
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Tiled grayscale: redirect writes to the strip scratch and clip to the
  // current band. Single predictable branch on the hot per-pixel path.
  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) {
      return;  // pixel outside the band currently being rendered
    }
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  std::string visualBuffer;
  const char* textCursor = resolveVisualText(text, visualBuffer, baseDir);

  // SD-card fonts can measure from their persistent advance table during layout.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor))) {
      widthFP += sdIt->second->getAdvance(cp, styleIdx);
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(textCursor, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(text, fontId, style);
    return;
  }

  std::string visualBuffer;
  const char* textCursor = resolveVisualText(text, visualBuffer, baseDir);

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }
  const auto& font = fontIt->second;

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, combiningGlyph->left,
                                                       combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, textCursor, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const auto metrics = getSyntheticSolidGlyphMetrics(font, style, cp);
      fillSyntheticSolidGlyph(*this, metrics, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const auto metrics = getSyntheticGreekGlyphMetrics(font, style, cp);
      drawSyntheticGreekGlyph(*this, metrics, cp, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const auto metrics = getSyntheticReplacementGlyphMetrics(font, style);
      drawSyntheticReplacementGlyph(*this, metrics, lastBaseX, yPos, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    if (!glyph) {
      // Advance was already flushed into lastBaseX above; clear base metrics so the
      // next character does not kern or attach to stale state.
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (isSupSub) {
      // Halve the advance so the cursor advances by the same amount the scaled glyph
      // actually occupies, keeping spacing correct without needing a separate smaller font.
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }

    if (isSupSub) {
      // yPos already carries the vertical offset applied by TextBlock::render().
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  const int sw = getScreenWidth();
  const int sh = getScreenHeight();
  if (x1 == x2) {
    if (x1 < 0 || x1 >= sw) return;
    if (y2 < y1) std::swap(y1, y2);
    y1 = std::max(y1, 0);
    y2 = std::min(y2, sh - 1);
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (y1 < 0 || y1 >= sh) return;
    if (x2 < x1) std::swap(x1, x2);
    x1 = std::max(x1, 0);
    x2 = std::min(x2, sw - 1);
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - i, y + i, state);
    drawLine(x + width - i, y + i, x + width - i, y + height - i, state);
    drawLine(x + width - i, y + height - i, x + i, y + height - i, state);
    drawLine(x + i, y + height - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  if (color == Color::Clear) {
  } else if (color == Color::Black) {
    fillRect(x, y, width, height, true);
  } else if (color == Color::White) {
    fillRect(x, y, width, height, false);
  } else if (color == Color::LightGray || color == Color::DarkGray) {
    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + width, getScreenWidth());
    const int y1 = std::min(y + height, getScreenHeight());
    if (x0 >= x1 || y0 >= y1) return;
    if (color == Color::LightGray) {
      for (int fillY = y0; fillY < y1; fillY++) {
        for (int fillX = x0; fillX < x1; fillX++) {
          drawPixelDither<Color::LightGray>(fillX, fillY);
        }
      }
    } else {
      for (int fillY = y0; fillY < y1; fillY++) {
        for (int fillX = x0; fillX < x1; fillX++) {
          drawPixelDither<Color::DarkGray>(fillX, fillY);
        }
      }
    }
  }
}

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawImageInverted(const uint8_t bitmap[], const int x, const int y, const int width,
                                    const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }

  const int imageWidthBytes = width / 8;
  if (!frameBuffer || imageWidthBytes <= 0 || rotatedX < 0 || rotatedY < 0) return;

  const int destByteX = rotatedX / 8;
  for (int row = 0; row < height; row++) {
    const int destY = rotatedY + row;
    if (destY >= panelHeight) break;

    const int destOffset = destY * panelWidthBytes + destByteX;
    const int srcOffset = row * imageWidthBytes;
    for (int col = 0; col < imageWidthBytes; col++) {
      if (destByteX + col >= panelWidthBytes) break;
      frameBuffer[destOffset + col] = static_cast<uint8_t>(~bitmap[srcOffset + col]);
    }
  }
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  drawIconPixels(*this, bitmap, x, y, width, height, true);
}

void GfxRenderer::drawIconInverted(const uint8_t bitmap[], const int x, const int y, const int width,
                                   const int height) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  drawIconPixels(*this, bitmap, x, y, width, height, false);
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it)
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }
}

void GfxRenderer::drawPerspectiveBitmap(const Bitmap& bitmap, const int x, const int y, const int w, const int hL,
                                        const int hR) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (w <= 0 || hL <= 0 || hR <= 0) return;

  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0) return;

  const int hMax = std::max(hL, hR);
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const bool topDown = bitmap.isTopDown();

  BitmapScratchLock scratchLock(*this);
  if (!scratchLock.isLocked()) return;

  const int outputRowSize = (srcW + 3) / 4;
  if (!ensureBitmapScratchBuffers(outputRowSize, bitmap.getRowBytes())) {
    return;
  }
  auto* outputRow = bitmapScratchOutputRow_;
  auto* rowBytes = bitmapScratchRowBytes_;

  for (int srcY = 0; srcY < srcH; srcY++) {
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap (perspective)", srcY);
      return;
    }
    const int srcRowIndex = topDown ? srcY : (srcH - 1 - srcY);

    for (int dx = 0; dx < w; dx++) {
      const int colH = (w == 1) ? hL : (hL + (hR - hL) * dx / (w - 1));
      if (colH <= 0) continue;
      const int colTop = (hMax - colH) / 2;
      const int screenX = x + dx;
      if (screenX < 0 || screenX >= screenW) continue;

      const int srcX = (dx * srcW) / w;
      const uint8_t val = (outputRow[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;

      const int dstYStart = (srcRowIndex * colH) / srcH;
      const int dstYEnd = ((srcRowIndex + 1) * colH) / srcH;
      for (int dy = dstYStart; dy < dstYEnd; ++dy) {
        const int screenY = y + colTop + dy;
        if (screenY < 0 || screenY >= screenH) continue;

        if (renderMode == BW && val < 3) {
          drawPixel(screenX, screenY);
        } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
          drawPixel(screenX, screenY, false);
        } else if (renderMode == GRAYSCALE_LSB && val == 1) {
          drawPixel(screenX, screenY, false);
        }
      }
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX, nodeX + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  if (_stripActive) {
    // Clear only the active band's scratch, not the shared framebuffer.
    memset(_stripBuf, color, static_cast<size_t>(panelWidthBytes) * _stripRows);
    return;
  }
  display.clearScreen(color);
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool turnOffScreen) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  display.displayBuffer(refreshMode, fadingFix || turnOffScreen);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    const auto fontIt = fontMap.find(fontId);
    if (fontIt == fontMap.end()) {
      LOG_ERR("GFX", "Font %d not found", fontId);
      return 0;
    }
    const auto& font = fontIt->second;
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      int32_t advFP = sdIt->second->getAdvance(cp, styleIdx);
      if (advFP == 0 && !utf8IsCombiningMark(cp)) {
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        advFP = glyph ? glyph->advanceX : 0;
      }
      widthFP += isSupSub ? (advFP + 1) / 2 : advFP;
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);         // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      prevAdvanceFP = getSyntheticSolidGlyphMetrics(font, style, cp).advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      prevAdvanceFP = getSyntheticGreekGlyphMetrics(font, style, cp).advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      prevAdvanceFP = getSyntheticReplacementGlyphMetrics(font, style).advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const int raiseBy = combiningMark::raiseAboveBase(combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::centerOverRotated90CW(lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);
    cp = font.getFallbackCodepoint(cp, style);
    const bool hasRealGlyph = font.findGlyphData(cp, style).glyph != nullptr;

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    if (!hasRealGlyph && syntheticGlyph::isSpaceFallback(cp)) {
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isSolid(cp)) {
      const auto metrics = getSyntheticSolidGlyphMetrics(font, style, cp);
      fillSyntheticSolidGlyphRotated90CW(*this, metrics, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isGreekFallback(cp)) {
      const auto metrics = getSyntheticGreekGlyphMetrics(font, style, cp);
      drawSyntheticGreekGlyphRotated90CW(*this, metrics, cp, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    if (!hasRealGlyph && syntheticGlyph::isReplacementFallback(cp)) {
      const auto metrics = getSyntheticReplacementGlyphMetrics(font, style);
      drawSyntheticReplacementGlyphRotated90CW(*this, metrics, x, lastBaseY, black);
      lastBaseLeft = metrics.left;
      lastBaseWidth = metrics.width;
      lastBaseTop = metrics.top;
      prevAdvanceFP = metrics.advanceX;
      prevCp = cp;
      continue;
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    if (!glyph) {
      // Advance was already flushed into lastBaseY above; clear base metrics so the
      // next character does not kern or attach to stale state.
      prevAdvanceFP = 0;
      lastBaseLeft = 0;
      lastBaseWidth = 0;
      lastBaseTop = 0;
      prevCp = 0;
      continue;
    }

    lastBaseLeft = glyph->left;
    lastBaseWidth = glyph->width;
    lastBaseTop = glyph->top;
    prevAdvanceFP = glyph->advanceX;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback, const bool turnOffScreen) const {
  display.displayGrayscaleBase(fallback, fadingFix || turnOffScreen);
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer(const bool turnOffScreen) const {
  display.displayGrayBuffer(fadingFix || turnOffScreen);
}

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return display.supportsStripGrayscale(); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
