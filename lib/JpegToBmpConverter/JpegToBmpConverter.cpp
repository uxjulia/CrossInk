#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <picojpeg.h>

#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"

// Context structure for picojpeg callback
struct JpegReadContext {
  FsFile& file;
  uint8_t buffer[512];
  size_t bufferPos;
  size_t bufferFilled;
};

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Callback function for picojpeg to read JPEG data
unsigned char JpegToBmpConverter::jpegReadCallback(unsigned char* pBuf, const unsigned char buf_size,
                                                   unsigned char* pBytes_actually_read, void* pCallback_data) {
  auto* context = static_cast<JpegReadContext*>(pCallback_data);

  if (!context || !context->file) {
    return PJPG_STREAM_READ_ERROR;
  }

  // Check if we need to refill our context buffer
  if (context->bufferPos >= context->bufferFilled) {
    context->bufferFilled = context->file.read(context->buffer, sizeof(context->buffer));
    context->bufferPos = 0;

    if (context->bufferFilled == 0) {
      // EOF or error
      *pBytes_actually_read = 0;
      return 0;  // Success (EOF is normal)
    }
  }

  // Copy available bytes to picojpeg's buffer
  const size_t available = context->bufferFilled - context->bufferPos;
  const size_t toRead = available < buf_size ? available : buf_size;

  memcpy(pBuf, context->buffer + context->bufferPos, toRead);
  context->bufferPos += toRead;
  *pBytes_actually_read = static_cast<unsigned char>(toRead);

  return 0;  // Success
}

// ============================================================================
// JPEGDEC-based fallback for progressive JPEGs (unsupported by picojpeg)
// ============================================================================

// Opens a fresh read-only FsFile by path so JPEGDEC gets an uncontaminated
// file handle.  The existing FsFile (partially consumed by picojpeg) is not
// reused — attempting seekSet(0) on it is unreliable after picojpeg reads
// ahead to detect the SOF2 marker.
static void* jpegDecBmpFileOpen(const char* filename, int32_t* size) {
  FsFile* f = new (std::nothrow) FsFile();
  if (!f) return nullptr;
  if (!Storage.openFileForRead("JPG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = static_cast<int32_t>(f->size());
  return f;
}
static void jpegDecBmpFileClose(void* handle) {
  FsFile* f = static_cast<FsFile*>(handle);
  f->close();
  delete f;
}
static int32_t jpegDecBmpFileRead(JPEGFILE* pFile, uint8_t* buf, int32_t len) {
  FsFile* f = static_cast<FsFile*>(pFile->fHandle);
  int32_t n = f->read(buf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}
static int32_t jpegDecBmpFileSeek(JPEGFILE* pFile, int32_t pos) {
  FsFile* f = static_cast<FsFile*>(pFile->fHandle);
  if (!f->seekSet(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

struct JpegDecBmpContext {
  Print* bmpOut;
  int scaledSrcWidth;
  int scaledSrcHeight;
  int outWidth;
  int outHeight;
  uint8_t* rowBuffer;
  int bytesPerRow;
  bool oneBit;
  AtkinsonDitherer* atkinsonDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;
  int lastFlushedOutY;
};

static int jpegDecBmpDrawCallback(JPEGDRAW* pDraw) {
  JpegDecBmpContext* ctx = static_cast<JpegDecBmpContext*>(pDraw->pUser);
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int blockY = pDraw->y;
  const int blockH = pDraw->iHeight;
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;

  for (int row = 0; row < blockH; row++) {
    const int srcY = blockY + row;
    if (srcY >= ctx->scaledSrcHeight) break;

    // Map source Y → output Y (nearest-neighbour)
    const int outY = static_cast<int>(static_cast<int64_t>(srcY) * ctx->outHeight / ctx->scaledSrcHeight);
    if (outY >= ctx->outHeight) break;
    if (outY <= ctx->lastFlushedOutY) continue;

    // Build output row with nearest-neighbour X mapping
    memset(ctx->rowBuffer, 0, ctx->bytesPerRow);
    const uint8_t* srcRow = pixels + row * stride;

    for (int outX = 0; outX < ctx->outWidth; outX++) {
      int srcX = static_cast<int>(static_cast<int64_t>(outX) * ctx->scaledSrcWidth / ctx->outWidth);
      if (srcX >= validW) srcX = validW - 1;
      const uint8_t gray = srcRow[srcX];

      if (ctx->oneBit) {
        const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, outX)
                                                      : quantize1bit(gray, outX, outY);
        ctx->rowBuffer[outX / 8] |= static_cast<uint8_t>(bit << (7 - (outX % 8)));
      } else {
        const uint8_t adj = adjustPixel(gray);
        const uint8_t twoBit =
            ctx->atkinsonDitherer ? ctx->atkinsonDitherer->processPixel(adj, outX) : quantize(adj, outX, outY);
        ctx->rowBuffer[(outX * 2) / 8] |= static_cast<uint8_t>(twoBit << (6 - ((outX * 2) % 8)));
      }
    }
    if (ctx->oneBit && ctx->atkinson1BitDitherer)
      ctx->atkinson1BitDitherer->nextRow();
    else if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();

    // For upscaling: repeat this row for any output rows between last flush and outY
    for (int oy = ctx->lastFlushedOutY + 1; oy <= outY; oy++) {
      ctx->bmpOut->write(ctx->rowBuffer, ctx->bytesPerRow);
    }
    ctx->lastFlushedOutY = outY;
  }
  return 1;
}

// Decodes a progressive (or any) JPEG to BMP using JPEGDEC when picojpeg
// has rejected the file.  Progressive JPEGs are decoded at 1/8 resolution
// (DC coefficients only), which is then scaled to the target dimensions.
static bool jpegFileToBmpStreamViaJpegDec(const char* filePath, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit) {
  if (!filePath) {
    LOG_ERR("JPG", "JPEGDEC fallback: no file path provided");
    return false;
  }
  constexpr size_t MIN_FREE_HEAP = 28 * 1024;  // ~20 KB decoder + 8 KB headroom
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEGDEC fallback (%u free)", ESP.getFreeHeap());
    return false;
  }

  JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "JPEGDEC fallback: failed to allocate decoder");
    return false;
  }

  // Pass filePath as the filename so jpegDecBmpFileOpen can open a fresh handle
  int rc = jpeg->open(filePath, jpegDecBmpFileOpen, jpegDecBmpFileClose, jpegDecBmpFileRead, jpegDecBmpFileSeek,
                      jpegDecBmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEGDEC fallback: open failed (err=%d)", jpeg->getLastError());
    delete jpeg;
    return false;
  }

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();

  // Progressive JPEGs are always decoded at 1/8 scale (DC coefficients only).
  // Baseline JPEGs reaching this path get the best fitting integer scale.
  const bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  int scaleOption;
  int scaleDenom;
  if (isProgressive) {
    scaleOption = JPEG_SCALE_EIGHTH;
    scaleDenom = 8;
  } else {
    const float ts = (targetWidth > 0 && targetHeight > 0) ? std::min(static_cast<float>(targetWidth) / srcWidth,
                                                                      static_cast<float>(targetHeight) / srcHeight)
                                                           : 1.0f;
    if (ts <= 0.125f) {
      scaleOption = JPEG_SCALE_EIGHTH;
      scaleDenom = 8;
    } else if (ts <= 0.25f) {
      scaleOption = JPEG_SCALE_QUARTER;
      scaleDenom = 4;
    } else if (ts <= 0.5f) {
      scaleOption = JPEG_SCALE_HALF;
      scaleDenom = 2;
    } else {
      scaleOption = 0;
      scaleDenom = 1;
    }
  }

  const int scaledSrcW = (srcWidth + scaleDenom - 1) / scaleDenom;
  const int scaledSrcH = (srcHeight + scaleDenom - 1) / scaleDenom;

  // Compute output dimensions preserving aspect ratio
  int outWidth = scaledSrcW;
  int outHeight = scaledSrcH;
  if (targetWidth > 0 && targetHeight > 0) {
    const float scaleW = static_cast<float>(targetWidth) / scaledSrcW;
    const float scaleH = static_cast<float>(targetHeight) / scaledSrcH;
    const float scale = std::min(scaleW, scaleH);
    outWidth = std::max(1, static_cast<int>(scaledSrcW * scale));
    outHeight = std::max(1, static_cast<int>(scaledSrcH * scale));
  }

  LOG_INF("JPG", "JPEGDEC fallback: %dx%d -> scaled %dx%d -> out %dx%d%s", srcWidth, srcHeight, scaledSrcW, scaledSrcH,
          outWidth, outHeight, isProgressive ? " [progressive/DC-only]" : "");

  // Write BMP header
  int bytesPerRow;
  if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR("JPG", "JPEGDEC fallback: failed to alloc row buffer");
    jpeg->close();
    delete jpeg;
    return false;
  }
  memset(rowBuffer, 0, bytesPerRow);

  AtkinsonDitherer* atkinsonDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;
  if (oneBit) {
    atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
  } else {
    atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);
  }

  JpegDecBmpContext ctx;
  ctx.bmpOut = &bmpOut;
  ctx.scaledSrcWidth = scaledSrcW;
  ctx.scaledSrcHeight = scaledSrcH;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.rowBuffer = rowBuffer;
  ctx.bytesPerRow = bytesPerRow;
  ctx.oneBit = oneBit;
  ctx.atkinsonDitherer = atkinsonDitherer;
  ctx.atkinson1BitDitherer = atkinson1BitDitherer;
  ctx.lastFlushedOutY = -1;

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  const bool success = jpeg->decode(0, 0, scaleOption) == 1;

  // Flush any remaining rows (source may end before full output height when upscaling)
  if (success) {
    for (int oy = ctx.lastFlushedOutY + 1; oy < outHeight; oy++) {
      bmpOut.write(ctx.rowBuffer, bytesPerRow);
    }
  }

  free(rowBuffer);
  delete atkinsonDitherer;
  delete atkinson1BitDitherer;
  jpeg->close();
  delete jpeg;

  if (!success) {
    LOG_ERR("JPG", "JPEGDEC fallback: decode failed");
  }
  return success;
}

// ============================================================================

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, const char* filePath) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // Setup context for picojpeg callback
  JpegReadContext context = {.file = jpegFile, .bufferPos = 0, .bufferFilled = 0};

  // Initialize picojpeg decoder
  pjpeg_image_info_t imageInfo;
  const unsigned char status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  if (status != 0) {
    if (status == PJPG_UNSUPPORTED_MODE) {
      // Progressive JPEG — picojpeg doesn't support these.  Fall back to JPEGDEC
      // which decodes progressive files using DC coefficients at 1/8 resolution.
      // Close the existing handle first: SdFat will not allow a second reader
      // on the same file while one is already open.
      LOG_INF("JPG", "Progressive JPEG detected, using JPEGDEC fallback decoder");
      jpegFile.close();
      return jpegFileToBmpStreamViaJpegDec(filePath, bmpOut, targetWidth, targetHeight, oneBit);
    }
    LOG_ERR("JPG", "JPEG decode init failed with error code: %d", status);
    return false;
  }

  LOG_DBG("JPG", "JPEG dimensions: %dx%d, components: %d, MCUs: %dx%d", imageInfo.m_width, imageInfo.m_height,
          imageInfo.m_comps, imageInfo.m_MCUSPerRow, imageInfo.m_MCUSPerCol);

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  constexpr int MAX_MCU_ROW_BYTES = 65536;

  if (imageInfo.m_width > MAX_IMAGE_WIDTH || imageInfo.m_height > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large (%dx%d), max supported: %dx%d", imageInfo.m_width, imageInfo.m_height,
            MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = imageInfo.m_width;
  int outHeight = imageInfo.m_height;
  // Use fixed-point scaling (16.16) for sub-pixel accuracy
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (imageInfo.m_width != targetWidth || imageInfo.m_height != targetHeight)) {
    // Calculate scale to fit/fill target dimensions while maintaining aspect ratio
    const float scaleToFitWidth = static_cast<float>(targetWidth) / imageInfo.m_width;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / imageInfo.m_height;
    // We scale to the smaller dimension, so we can potentially crop later.
    float scale = 1.0;
    if (crop) {  // if we will crop, scale to the smaller dimension
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {  // else, scale to the larger dimension to fit
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(imageInfo.m_width * scale);
    outHeight = static_cast<int>(imageInfo.m_height * scale);

    // Ensure at least 1 pixel
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    // Calculate fixed-point scale factors (source pixels per output pixel)
    // scaleX_fp = (srcWidth << 16) / outWidth
    scaleX_fp = (static_cast<uint32_t>(imageInfo.m_width) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(imageInfo.m_height) << 16) / outHeight;
    needsScaling = true;

    LOG_DBG("JPG", "Scaling %dx%d -> %dx%d (target %dx%d)", imageInfo.m_width, imageInfo.m_height, outWidth, outHeight,
            targetWidth, targetHeight);
  }

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;  // 1 bit per pixel
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  uint8_t* rowBuffer = nullptr;
  uint8_t* mcuRowBuffer = nullptr;
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;
  uint32_t* rowAccum = nullptr;  // Accumulator for each output X (32-bit for larger sums)
  uint32_t* rowCount = nullptr;  // Count of source pixels accumulated per output X

  // RAII guard: frees all heap resources on any return path, including early exits.
  // Holds references so it always sees the latest pointer values assigned below.
  struct Cleanup {
    uint8_t*& rowBuffer;
    uint8_t*& mcuRowBuffer;
    AtkinsonDitherer*& atkinsonDitherer;
    FloydSteinbergDitherer*& fsDitherer;
    Atkinson1BitDitherer*& atkinson1BitDitherer;
    uint32_t*& rowAccum;
    uint32_t*& rowCount;
    ~Cleanup() {
      delete[] rowAccum;
      delete[] rowCount;
      delete atkinsonDitherer;
      delete fsDitherer;
      delete atkinson1BitDitherer;
      free(mcuRowBuffer);
      free(rowBuffer);
    }
  } cleanup{rowBuffer, mcuRowBuffer, atkinsonDitherer, fsDitherer, atkinson1BitDitherer, rowAccum, rowCount};

  // Allocate row buffer
  rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR("JPG", "Failed to allocate row buffer");
    return false;
  }

  // Allocate a buffer for one MCU row worth of grayscale pixels
  // This is the minimal memory needed for streaming conversion
  const int mcuPixelHeight = imageInfo.m_MCUHeight;
  const int mcuRowPixels = imageInfo.m_width * mcuPixelHeight;

  // Validate MCU row buffer size before allocation
  if (mcuRowPixels > MAX_MCU_ROW_BYTES) {
    LOG_DBG("JPG", "MCU row buffer too large (%d bytes), max: %d", mcuRowPixels, MAX_MCU_ROW_BYTES);
    return false;
  }

  mcuRowBuffer = static_cast<uint8_t*>(malloc(mcuRowPixels));
  if (!mcuRowBuffer) {
    LOG_ERR("JPG", "Failed to allocate MCU row buffer (%d bytes)", mcuRowPixels);
    return false;
  }

  // Create ditherer if enabled
  // Use OUTPUT dimensions for dithering (after prescaling)
  if (oneBit) {
    // For 1-bit output, use Atkinson dithering for better quality
    atkinson1BitDitherer = new Atkinson1BitDitherer(outWidth);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = new FloydSteinbergDitherer(outWidth);
    }
  }

  // For scaling: accumulate source rows into scaled output rows
  // We need to track which source Y maps to which output Y
  // Using fixed-point: srcY_fp = outY * scaleY_fp (gives source Y in 16.16 format)
  int currentOutY = 0;             // Current output row being accumulated
  uint32_t nextOutY_srcStart = 0;  // Source Y where next output row starts (16.16 fixed point)

  if (needsScaling) {
    rowAccum = new uint32_t[outWidth]();
    rowCount = new uint32_t[outWidth]();
    nextOutY_srcStart = scaleY_fp;  // First boundary is at scaleY_fp (source Y for outY=1)
  }

  // Process MCUs row-by-row and write to BMP as we go (top-down)
  const int mcuPixelWidth = imageInfo.m_MCUWidth;

  for (int mcuY = 0; mcuY < imageInfo.m_MCUSPerCol; mcuY++) {
    // Clear the MCU row buffer
    memset(mcuRowBuffer, 0, mcuRowPixels);

    // Decode one row of MCUs
    for (int mcuX = 0; mcuX < imageInfo.m_MCUSPerRow; mcuX++) {
      const unsigned char mcuStatus = pjpeg_decode_mcu();
      if (mcuStatus != 0) {
        if (mcuStatus == PJPG_NO_MORE_BLOCKS) {
          LOG_ERR("JPG", "Unexpected end of blocks at MCU (%d, %d)", mcuX, mcuY);
        } else {
          LOG_ERR("JPG", "JPEG decode MCU failed at (%d, %d) with error code: %d", mcuX, mcuY, mcuStatus);
        }
        return false;
      }

      // picojpeg stores MCU data in 8x8 blocks
      // Block layout: H2V2(16x16)=0,64,128,192 H2V1(16x8)=0,64 H1V2(8x16)=0,128
      for (int blockY = 0; blockY < mcuPixelHeight; blockY++) {
        for (int blockX = 0; blockX < mcuPixelWidth; blockX++) {
          const int pixelX = mcuX * mcuPixelWidth + blockX;
          if (pixelX >= imageInfo.m_width) continue;

          // Calculate proper block offset for picojpeg buffer
          const int blockCol = blockX / 8;
          const int blockRow = blockY / 8;
          const int localX = blockX % 8;
          const int localY = blockY % 8;
          const int blocksPerRow = mcuPixelWidth / 8;
          const int blockIndex = blockRow * blocksPerRow + blockCol;
          const int pixelOffset = blockIndex * 64 + localY * 8 + localX;

          uint8_t gray;
          if (imageInfo.m_comps == 1) {
            gray = imageInfo.m_pMCUBufR[pixelOffset];
          } else {
            const uint8_t r = imageInfo.m_pMCUBufR[pixelOffset];
            const uint8_t g = imageInfo.m_pMCUBufG[pixelOffset];
            const uint8_t b = imageInfo.m_pMCUBufB[pixelOffset];
            gray = (r * 25 + g * 50 + b * 25) / 100;
          }

          mcuRowBuffer[blockY * imageInfo.m_width + pixelX] = gray;
        }
      }
    }

    // Process source rows from this MCU row
    const int startRow = mcuY * mcuPixelHeight;
    const int endRow = (mcuY + 1) * mcuPixelHeight;

    for (int y = startRow; y < endRow && y < imageInfo.m_height; y++) {
      const int bufferY = y - startRow;

      if (!needsScaling) {
        // No scaling - direct output (1:1 mapping)
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          // 1-bit output with Atkinson dithering for better quality
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, y);
            // Pack 1-bit value: MSB first, 8 pixels per byte
            const int byteIndex = x / 8;
            const int bitOffset = 7 - (x % 8);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          // 2-bit output
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel(mcuRowBuffer[bufferY * imageInfo.m_width + x]);
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, y);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }
        bmpOut.write(rowBuffer, bytesPerRow);
      } else {
        // Fixed-point area averaging for exact fit scaling
        // For each output pixel X, accumulate source pixels that map to it
        // srcX range for outX: [outX * scaleX_fp >> 16, (outX+1) * scaleX_fp >> 16)
        const uint8_t* srcRow = mcuRowBuffer + bufferY * imageInfo.m_width;

        for (int outX = 0; outX < outWidth; outX++) {
          // Calculate source X range for this output pixel
          const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
          const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

          // Accumulate all source pixels in this range
          int sum = 0;
          int count = 0;
          for (int srcX = srcXStart; srcX < srcXEnd && srcX < imageInfo.m_width; srcX++) {
            sum += srcRow[srcX];
            count++;
          }

          // Handle edge case: if no pixels in range, use nearest
          if (count == 0 && srcXStart < imageInfo.m_width) {
            sum = srcRow[srcXStart];
            count = 1;
          }

          rowAccum[outX] += sum;
          rowCount[outX] += count;
        }

        // Check if we've crossed into the next output row(s)
        // Current source Y in fixed point: y << 16
        const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

        // Output all rows whose boundaries we've crossed (handles both up and downscaling)
        // For upscaling, one source row may produce multiple output rows
        while (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
          memset(rowBuffer, 0, bytesPerRow);

          if (USE_8BIT_OUTPUT && !oneBit) {
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              rowBuffer[x] = adjustPixel(gray);
            }
          } else if (oneBit) {
            // 1-bit output with Atkinson dithering for better quality
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              const uint8_t bit = atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x)
                                                       : quantize1bit(gray, x, currentOutY);
              // Pack 1-bit value: MSB first, 8 pixels per byte
              const int byteIndex = x / 8;
              const int bitOffset = 7 - (x % 8);
              rowBuffer[byteIndex] |= (bit << bitOffset);
            }
            if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
          } else {
            // 2-bit output
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
              uint8_t twoBit;
              if (atkinsonDitherer) {
                twoBit = atkinsonDitherer->processPixel(gray, x);
              } else if (fsDitherer) {
                twoBit = fsDitherer->processPixel(gray, x);
              } else {
                twoBit = quantize(gray, x, currentOutY);
              }
              const int byteIndex = (x * 2) / 8;
              const int bitOffset = 6 - ((x * 2) % 8);
              rowBuffer[byteIndex] |= (twoBit << bitOffset);
            }
            if (atkinsonDitherer)
              atkinsonDitherer->nextRow();
            else if (fsDitherer)
              fsDitherer->nextRow();
          }

          bmpOut.write(rowBuffer, bytesPerRow);
          currentOutY++;

          // Update boundary for next output row
          nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;

          // For upscaling: don't reset accumulators if next output row uses same source data
          // Only reset when we'll move to a new source row
          if (srcY_fp >= nextOutY_srcStart) {
            // More output rows to emit from same source - keep accumulator data
            continue;
          }
          // Moving to next source row - reset accumulators
          memset(rowAccum, 0, outWidth * sizeof(uint32_t));
          memset(rowCount, 0, outWidth * sizeof(uint32_t));
        }
      }
    }
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, const char* filePath,
                                                     int targetMaxWidth, int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, true, filePath);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, const char* filePath,
                                                         int targetMaxWidth, int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, filePath);
}
