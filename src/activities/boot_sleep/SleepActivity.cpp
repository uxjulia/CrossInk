#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PNGdec.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <functional>
#include <new>

#include "../reader/BookStatsView.h"
#include "../reader/EpubReaderActivity.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {

constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;

// Re-draws the "Entering sleep" popup into the cleared framebuffer just before the
// HALF_REFRESH preflash inside renderGrayscaleSinglePass. Without this, the popup's
// black pixels are forced to flip to white during the preflash on top of the FAST_REFRESH
// residue from GUI.drawPopup, leaving gray dirt in the wallpaper's white areas.
// Mirrors upstream crosspoint-reader PR#1614.
void drawEnteringSleepOverlay(const GfxRenderer& r, const void*) {
  constexpr int margin = 15;
  const char* msg = tr(STR_ENTERING_SLEEP);
  const int y = static_cast<int>(r.getScreenHeight() * 0.075f);
  const int textWidth = r.getTextWidth(UI_12_FONT_ID, msg, EpdFontFamily::BOLD);
  const int w = textWidth + margin * 2;
  const int h = r.getLineHeight(UI_12_FONT_ID) + margin * 2;
  const int x = (r.getScreenWidth() - w) / 2;
  r.fillRect(x - 2, y - 2, w + 4, h + 4, true);
  r.fillRect(x, y, w, h, false);
  r.drawText(UI_12_FONT_ID, x + margin, y + margin - 2, msg, true, EpdFontFamily::BOLD);
}

void hideOverlayBatteryStrip(const GfxRenderer& renderer) {
  if (!SETTINGS.statusBarBattery) {
    return;
  }

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  const int textY = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - 4;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Reserve the full left-side status indicator lane used by bookmark + battery.
  // This keeps chapter/progress text readable while removing the battery glance target.
  static constexpr int bookmarkReserveWidth = 13;  // bookmark width + gap from BaseTheme::drawStatusBar()
  static constexpr int batteryPercentSpacing = 4;  // matches BaseTheme::batteryPercentSpacing
  const int clearWidth =
      bookmarkReserveWidth + metrics.batteryWidth +
      (showBatteryPercentage ? batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, "100%") : 0);
  const int clearHeight = std::max(renderer.getTextHeight(SMALL_FONT_ID), metrics.batteryHeight + 6);

  renderer.fillRect(metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, clearWidth, clearHeight, false);
}

// Context passed through PNGdec's decode() user-pointer to the per-scanline draw callback.
struct PngOverlayCtx {
  const GfxRenderer* renderer;
  int screenW;
  int screenH;
  int srcWidth;
  int dstWidth;
  int dstX;
  int dstY;
  float yScale;
  int lastDstY;
  // Color-key transparency (tRNS chunk) for TRUECOLOR and GRAYSCALE images.
  // Initialized lazily on the first draw callback because tRNS is processed during decode(),
  // not during open() — so hasAlpha()/getTransparentColor() are only valid once decode() starts.
  // -2 = not yet read; -1 = no color key; >=0 = 0x00RRGGBB (TRUECOLOR) or low-byte gray.
  int32_t transparentColor;
  PNG* pngObj;  // for lazy-init of transparentColor on first callback
};

// PNGdec file I/O callbacks — mirror the pattern in PngToFramebufferConverter.cpp.
void* pngSleepOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("SLP", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}
void pngSleepClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}
int32_t pngSleepRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  return f ? f->read(pBuf, len) : 0;
}
int32_t pngSleepSeek(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Per-scanline draw callback for PNG overlay compositing.
// Transparent pixels (alpha < 128) are skipped so the reader page shows through.
// Opaque pixels are drawn in their grayscale brightness (dark → black, light → white).
int pngOverlayDraw(PNGDRAW* pDraw) {
  PngOverlayCtx* ctx = reinterpret_cast<PngOverlayCtx*>(pDraw->pUser);

  // Lazy-init: tRNS chunk is processed during decode() before any IDAT data, so by the time
  // the first draw callback fires, hasAlpha() / getTransparentColor() are already valid.
  if (ctx->transparentColor == -2) {
    const int pt = pDraw->iPixelType;
    ctx->transparentColor = (pDraw->iHasAlpha && (pt == PNG_PIXEL_TRUECOLOR || pt == PNG_PIXEL_GRAYSCALE))
                                ? static_cast<int32_t>(ctx->pngObj->getTransparentColor())
                                : -1;
  }

  const int destY = ctx->dstY + (int)(pDraw->y * ctx->yScale);
  if (destY == ctx->lastDstY) return 1;  // skip duplicate rows from Y scaling
  ctx->lastDstY = destY;
  if (destY < 0 || destY >= ctx->screenH) return 1;

  const int srcWidth = ctx->srcWidth;
  const int dstWidth = ctx->dstWidth;
  const uint8_t* pixels = pDraw->pPixels;
  const int pixelType = pDraw->iPixelType;
  const int hasAlpha = pDraw->iHasAlpha;

  int srcX = 0, error = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = ctx->dstX + dstX;
    if (outX >= 0 && outX < ctx->screenW) {
      uint8_t alpha = 255, gray = 0;
      switch (pixelType) {
        case PNG_PIXEL_TRUECOLOR_ALPHA: {
          const uint8_t* p = &pixels[srcX * 4];
          alpha = p[3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          break;
        }
        case PNG_PIXEL_GRAY_ALPHA:
          gray = pixels[srcX * 2];
          alpha = pixels[srcX * 2 + 1];
          break;
        case PNG_PIXEL_TRUECOLOR: {
          const uint8_t* p = &pixels[srcX * 3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          // tRNS color-key: if pixel matches the designated transparent color, skip it
          if (ctx->transparentColor >= 0 && p[0] == (uint8_t)((ctx->transparentColor >> 16) & 0xFF) &&
              p[1] == (uint8_t)((ctx->transparentColor >> 8) & 0xFF) &&
              p[2] == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        }
        case PNG_PIXEL_GRAYSCALE:
          gray = pixels[srcX];
          // tRNS color-key: transparent gray value stored in low byte
          if (ctx->transparentColor >= 0 && gray == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        case PNG_PIXEL_INDEXED:
          if (pDraw->pPalette) {
            const uint8_t idx = pixels[srcX];
            const uint8_t* p = &pDraw->pPalette[idx * 3];
            gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            if (hasAlpha) alpha = pDraw->pPalette[768 + idx];
          }
          break;
        default:
          gray = pixels[srcX];
          break;
      }

      if (alpha >= 128) {
        ctx->renderer->drawPixel(outX, destY, gray < 128);  // true = black, false = white
      }
      // alpha < 128: transparent — leave the reader page pixel intact
    }

    // Bresenham-style X stepping (handles downscaling; 1:1 when srcWidth == dstWidth)
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }
  return 1;
}

std::string filenameFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
}

std::string recentTitleForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book = std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) {
    return candidate.path == path && !candidate.title.empty();
  });
  return book == books.end() ? std::string{} : book->title;
}

enum class OverlayDrawResult : uint8_t { NotFound, Drawn, Failed };

enum class SleepImageMode : uint8_t { Custom, Overlay };

struct SleepImageSelection {
  std::string path;
  bool isPng = false;
  bool isPxc = false;
};

bool isBmpSleepImagePath(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool isPngSleepImagePath(const std::string& path) { return FsHelpers::hasPngExtension(path); }

bool openPreferredSleepDirectory(FsFile& dir, const char*& sleepDir) {
  sleepDir = nullptr;
  dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
    return true;
  }

  if (dir) dir.close();
  dir = Storage.open("/sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/sleep";
    return true;
  }

  if (dir) dir.close();
  return false;
}

bool selectPinnedSleepImage(SleepImageMode mode, SleepImageSelection& selection) {
  const std::string& favorite = APP_STATE.favoriteSleepImagePath;
  if (favorite.empty()) {
    return false;
  }

  if (!Storage.exists(favorite.c_str())) {
    LOG_INF("SLP", "Pinned sleep image missing, falling back: %s", favorite.c_str());
    return false;
  }

  if (isBmpSleepImagePath(favorite)) {
    selection.path = favorite;
    selection.isPng = false;
    selection.isPxc = false;
    return true;
  }

  if (FsHelpers::hasPxcExtension(favorite)) {
    if (mode == SleepImageMode::Custom) {
      selection.path = favorite;
      selection.isPng = false;
      selection.isPxc = true;
      return true;
    }

    LOG_INF("SLP", "Pinned PXC sleep image requires Custom mode, falling back: %s", favorite.c_str());
    return false;
  }

  if (isPngSleepImagePath(favorite)) {
    if (mode == SleepImageMode::Overlay) {
      selection.path = favorite;
      selection.isPng = true;
      selection.isPxc = false;
      return true;
    }

    LOG_INF("SLP", "Pinned PNG sleep image requires Page Overlay mode, falling back: %s", favorite.c_str());
    return false;
  }

  LOG_ERR("SLP", "Pinned sleep image has unsupported extension: %s", favorite.c_str());
  return false;
}

bool selectRandomSleepImage(SleepImageMode mode, SleepImageSelection& selection) {
  FsFile dir;
  const char* sleepDir = nullptr;
  if (!openPreferredSleepDirectory(dir, sleepDir)) {
    return false;
  }

  const bool allowPng = mode == SleepImageMode::Overlay;
  const bool allowPxc = mode == SleepImageMode::Custom;
  std::vector<std::string> files;
  files.reserve(16);
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
    const bool isPxc = allowPxc && FsHelpers::hasPxcExtension(filename);
    if (!isBmp && !isPng && !isPxc) {
      file.close();
      continue;
    }

    if (isBmp) {
      Bitmap bitmap(file);
      const BmpReaderError parseResult = bitmap.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "Skipping invalid BMP sleep image %s/%s: %s", sleepDir, filename.c_str(),
                Bitmap::errorToString(parseResult));
        file.close();
        continue;
      }
    }
    if (isPxc) {
      uint16_t width, height;
      if (file.read(&width, 2) != 2 || file.read(&height, 2) != 2) {
        LOG_ERR("SLP", "Skipping PXC sleep image with unreadable header %s/%s", sleepDir, filename.c_str());
        file.close();
        continue;
      }
    }

    files.emplace_back(std::move(filename));
    file.close();
  }
  dir.close();

  if (files.empty()) {
    return false;
  }

  const uint16_t fileCount = static_cast<uint16_t>(std::min(files.size(), static_cast<size_t>(UINT16_MAX)));
  const uint8_t window =
      static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), files.size() - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  APP_STATE.pushRecentSleep(randomFileIndex);
  APP_STATE.saveToFile();
  selection.path = std::string(sleepDir) + "/" + files[randomFileIndex];
  selection.isPng = FsHelpers::hasPngExtension(selection.path);
  selection.isPxc = FsHelpers::hasPxcExtension(selection.path);
  return true;
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  overlayPageBufferStored = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY &&
                            APP_STATE.lastSleepFromReader && renderer.storeBwBuffer();
  overlayPageBufferTrusted = overlayPageBufferStored && canSnapshotOverlayBackground;

  // Show the popup in the reader's orientation when sleep starts from an open book.
  // Reset to portrait afterwards so the sleep screen renderer keeps its existing layout.
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY):
      return renderOverlaySleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::READING_STATS_SLEEP):
      return renderReadingStatsSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  SleepImageSelection selection;
  if (selectPinnedSleepImage(SleepImageMode::Custom, selection) ||
      selectRandomSleepImage(SleepImageMode::Custom, selection)) {
    LOG_INF("SLP", "Loading custom sleep image: %s", selection.path.c_str());
    delay(100);
    if (selection.isPxc) {
      if (renderPxcSleepScreen(selection.path)) {
        return;
      }
    } else {
      FsFile file;
      if (Storage.openFileForRead("SLP", selection.path, file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          if (bitmap.hasGreyscale() &&
              SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
            lastGrayscalePath = selection.path;
            lastGrayscaleIsPxc = false;
          }
          renderBitmapSleepScreen(bitmap);
          return;
        }
        LOG_ERR("SLP", "Failed to parse custom sleep BMP: %s", selection.path.c_str());
      } else {
        LOG_ERR("SLP", "Failed to open custom sleep image: %s", selection.path.c_str());
      }
    }
  }

  if (Storage.exists("/sleep.pxc")) {
    LOG_DBG("SLP", "Loading: /sleep.pxc");
    if (renderPxcSleepScreen("/sleep.pxc")) {
      return;
    }
  }

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      if (bitmap.hasGreyscale() &&
          SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
        lastGrayscalePath = "/sleep.bmp";
        lastGrayscaleIsPxc = false;
      }
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSINK), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

bool SleepActivity::renderPxcSleepScreen(const std::string& path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    LOG_ERR("SLP", "Cannot open PXC: %s", path.c_str());
    return false;
  }

  uint16_t pxcWidth, pxcHeight;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("SLP", "PXC header read failed: %s", path.c_str());
    file.close();
    return false;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (pxcWidth != screenWidth || pxcHeight != screenHeight) {
    LOG_ERR("SLP", "PXC size %dx%d does not match screen %dx%d", pxcWidth, pxcHeight, screenWidth, screenHeight);
    file.close();
    return false;
  }

  const uint32_t dataOffset = file.position();  // right after the 4-byte header
  const auto filter = SETTINGS.sleepScreenCoverFilter;
  const int bytesPerRow = (pxcWidth + 3) / 4;

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
    lastGrayscalePath = path;
    lastGrayscaleIsPxc = true;
    struct PxcCtx {
      FsFile* file;
      uint32_t dataOffset;
      int width, height;
    };
    PxcCtx ctx{&file, dataOffset, pxcWidth, pxcHeight};

    renderer.renderGrayscaleSinglePass(
        GfxRenderer::GrayscaleMode::FactoryQuality,
        [](const GfxRenderer& r, const void* raw) {
          const auto* c = static_cast<const PxcCtx*>(raw);
          c->file->seek(c->dataOffset);

          const int bpr = (c->width + 3) / 4;
          uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bpr));
          if (!rowBuf) {
            LOG_ERR("SLP", "malloc failed for rowBuf (%d bytes, %dx%d)", bpr, c->width, c->height);
            return;
          }

          DirectPixelWriter pw;
          pw.init(r);

          for (int row = 0; row < c->height; row++) {
            if (c->file->read(rowBuf, bpr) != bpr) break;
            pw.beginRow(row);
            for (int col = 0; col < c->width; col++) {
              const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
              pw.writePixel(pv);
            }
          }
          free(rowBuf);
        },
        &ctx, &drawEnteringSleepOverlay, nullptr, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  } else {
    // BLACK_AND_WHITE / INVERTED_BLACK_AND_WHITE: threshold PXC to 1-bit
    // (pv 0=Black, 1=DarkGrey map to dark; 2=LightGrey, 3=White map to light)
    renderer.clearScreen();
    if (!file.seek(dataOffset)) {
      LOG_ERR("SLP", "PXC seek failed: %s", path.c_str());
      file.close();
      return false;
    }

    uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
    if (!rowBuf) {
      LOG_ERR("SLP", "PXC malloc failed");
      file.close();
      return false;
    }

    for (int row = 0; row < pxcHeight; row++) {
      if (file.read(rowBuf, bytesPerRow) != bytesPerRow) break;
      for (int col = 0; col < pxcWidth; col++) {
        const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
        if (pv < 2) renderer.drawPixel(col, row, true);
      }
    }
    free(rowBuf);

    if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  }

  file.close();
  return true;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  if (hasGreyscale) {
    struct BitmapGrayCtx {
      const Bitmap* bitmap;
      int x, y, maxWidth, maxHeight;
      float cropX, cropY;
    };
    BitmapGrayCtx grayCtx{&bitmap, x, y, pageWidth, pageHeight, cropX, cropY};
    renderer.renderGrayscaleSinglePass(
        GfxRenderer::GrayscaleMode::FactoryQuality,
        [](const GfxRenderer& r, const void* raw) {
          const auto* c = static_cast<const BitmapGrayCtx*>(raw);
          r.drawBitmap(*c->bitmap, c->x, c->y, c->maxWidth, c->maxHeight, c->cropX, c->cropY);
        },
        &grayCtx, &drawEnteringSleepOverlay, nullptr, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  } else {
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (lastXtc.getBitDepth() == 2) {
      const size_t planeSize = (static_cast<size_t>(lastXtc.getPageWidth()) * lastXtc.getPageHeight() + 7) / 8;
      uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane1) {
        LOG_ERR("SLP", "Failed to alloc plane1 for direct XTCH render (%lu bytes)",
                static_cast<unsigned long>(planeSize));
        return (this->*renderNoCoverSleepScreen)();
      }
      uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane2) {
        LOG_ERR("SLP", "Failed to alloc plane2 for direct XTCH render (%lu bytes)",
                static_cast<unsigned long>(planeSize));
        free(plane1);
        return (this->*renderNoCoverSleepScreen)();
      }

      if (lastXtc.loadPageMsb(0, plane1, planeSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTCH plane1 for sleep cover");
        free(plane1);
        free(plane2);
        return (this->*renderNoCoverSleepScreen)();
      }
      if (lastXtc.loadPageLsb(0, plane2, planeSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTCH plane2 for sleep cover");
        free(plane1);
        free(plane2);
        return (this->*renderNoCoverSleepScreen)();
      }

      LOG_DBG("SLP", "Direct XTCH plane render: %ux%u", lastXtc.getPageWidth(), lastXtc.getPageHeight());
      if (!APP_STATE.lastSleepFromReader) {
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.displayXtchPlanes(plane1, plane2, lastXtc.getPageWidth(), lastXtc.getPageHeight());
      free(plane1);
      free(plane2);
      return;
    }

    if (lastXtc.getBitDepth() == 1) {
      const size_t bufferSize = (static_cast<size_t>(lastXtc.getPageWidth() + 7) / 8) * lastXtc.getPageHeight();
      uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bufferSize));
      if (!pageBuffer) {
        LOG_ERR("SLP", "Failed to alloc page buffer for direct XTC render (%lu bytes)",
                static_cast<unsigned long>(bufferSize));
        return (this->*renderNoCoverSleepScreen)();
      }
      if (lastXtc.loadPage(0, pageBuffer, bufferSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTC page for sleep cover");
        free(pageBuffer);
        return (this->*renderNoCoverSleepScreen)();
      }
      LOG_DBG("SLP", "Direct XTC page render: %ux%u", lastXtc.getPageWidth(), lastXtc.getPageHeight());
      if (!APP_STATE.lastSleepFromReader) {
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.displayXtcBwPage(pageBuffer, lastXtc.getPageWidth(), lastXtc.getPageHeight());
      free(pageBuffer);
      return;
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      if (bitmap.hasGreyscale() &&
          SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
        lastGrayscalePath = coverBmpPath;
        lastGrayscaleIsPxc = false;
      }
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderReadingStatsSleepScreen() const {
  BookReadingStats bookStats;
  GlobalReadingStats globalStats = GlobalReadingStats::load();
  std::string bookTitle = tr(STR_READING_STATS);

  const std::string& path = APP_STATE.openEpubPath;
  if (!path.empty()) {
    const std::string recentTitle = recentTitleForPath(path);
    bookTitle = recentTitle.empty() ? filenameFromPath(path) : recentTitle;

    if (FsHelpers::hasEpubExtension(path)) {
      const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
      bookStats = BookReadingStats::load(cachePath);
    }
  }

  renderBookStatsView(renderer, nullptr, bookTitle, bookStats, globalStats, false);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& path = APP_STATE.openEpubPath;

  auto renderSavedReaderPage = [&]() -> bool {
    if (path.empty()) {
      return false;
    }

    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      return XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".txt")) {
      return TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    if (FsHelpers::checkFileExtension(path, ".epub")) {
      return EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }
    return false;
  };
  const bool backgroundSupportsGrayscale =
      FsHelpers::checkFileExtension(path, ".txt") || FsHelpers::checkFileExtension(path, ".epub");
  bool backgroundWasRebuilt = false;

  // Step 1: Ensure the frame buffer contains only the reader page.
  // When sleeping from the reader, restore the page snapshot taken before the
  // popup was drawn. Otherwise, rebuild from the saved position.
  if (overlayPageBufferTrusted) {
    renderer.restoreBwBuffer();
  } else if (!path.empty()) {
    backgroundWasRebuilt = renderSavedReaderPage();

    if (!backgroundWasRebuilt) {
      if (overlayPageBufferStored) {
        LOG_DBG("SLP", "Page re-render failed, using captured screen as overlay fallback");
        renderer.restoreBwBuffer();
      } else {
        LOG_DBG("SLP", "Page re-render failed, using white background");
        renderer.clearScreen();
      }
    }
  } else {
    renderer.clearScreen();
  }

  // Remove the live battery strip from the preserved/reconstructed reader page so the
  // overlay sleep screen still shows chapter/progress details without the battery glance target.
  hideOverlayBatteryStrip(renderer);

  // Step 2: Load the overlay image using the same selection logic as renderCustomSleepScreen.
  // BMP: white pixels are skipped (transparent via drawBitmap), black pixels composited on top.
  // PNG: pixels with alpha < 128 are skipped; opaque pixels are drawn with their grayscale value.
  auto tryDrawOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) {
      if (Storage.exists(filename.c_str())) {
        LOG_ERR("SLP", "BMP overlay exists but could not be opened: %s", filename.c_str());
        return OverlayDrawResult::Failed;
      }
      LOG_DBG("SLP", "BMP overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }
    Bitmap bitmap(file, true);
    const BmpReaderError parseResult = bitmap.parseHeaders();
    if (parseResult != BmpReaderError::Ok) {
      LOG_ERR("SLP", "BMP overlay header parse failed for %s: %s", filename.c_str(),
              Bitmap::errorToString(parseResult));
      file.close();
      return OverlayDrawResult::Failed;
    }

    int x, y;
    float cropX = 0, cropY = 0;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    // Draw without clearScreen so the reader page remains in the frame buffer beneath
    LOG_INF("SLP", "Drawing BMP overlay: %s", filename.c_str());
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    file.close();
    return OverlayDrawResult::Drawn;
  };

  auto tryDrawPngOverlay = [&](const std::string& filename) -> OverlayDrawResult {
    if (!Storage.exists(filename.c_str())) {
      LOG_DBG("SLP", "PNG overlay not found: %s", filename.c_str());
      return OverlayDrawResult::NotFound;
    }

    constexpr size_t MIN_FREE_HEAP = 60 * 1024;  // PNG decoder ~42 KB + overhead
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      LOG_ERR("SLP", "Not enough heap for PNG overlay decoder: %u free, need %u for %s", ESP.getFreeHeap(),
              static_cast<unsigned>(MIN_FREE_HEAP), filename.c_str());
      return OverlayDrawResult::Failed;
    }
    PNG* png = new (std::nothrow) PNG();
    if (!png) {
      LOG_ERR("SLP", "Failed to allocate PNG overlay decoder for %s", filename.c_str());
      return OverlayDrawResult::Failed;
    }

    int rc = png->open(filename.c_str(), pngSleepOpen, pngSleepClose, pngSleepRead, pngSleepSeek, pngOverlayDraw);
    if (rc != PNG_SUCCESS) {
      delete png;
      LOG_ERR("SLP", "PNG overlay open failed for %s: %d", filename.c_str(), rc);
      return OverlayDrawResult::Failed;
    }

    const int srcW = png->getWidth(), srcH = png->getHeight();
    float yScale = 1.0f;
    int dstW = srcW, dstH = srcH;
    if (srcW > pageWidth || srcH > pageHeight) {
      const float scaleX = (float)pageWidth / srcW, scaleY = (float)pageHeight / srcH;
      const float scale = (scaleX < scaleY) ? scaleX : scaleY;
      dstW = (int)(srcW * scale);
      dstH = (int)(srcH * scale);
      yScale = (float)dstH / srcH;
    }

    PngOverlayCtx ctx;
    ctx.renderer = &renderer;
    ctx.screenW = pageWidth;
    ctx.screenH = pageHeight;
    ctx.srcWidth = srcW;
    ctx.dstWidth = dstW;
    ctx.dstX = (pageWidth - dstW) / 2;
    ctx.dstY = (pageHeight - dstH) / 2;
    ctx.yScale = yScale;
    ctx.lastDstY = -1;
    ctx.transparentColor = -2;  // will be resolved on first draw callback (after tRNS is parsed)
    ctx.pngObj = png;

    LOG_INF("SLP", "Drawing PNG overlay: %s", filename.c_str());
    rc = png->decode(&ctx, 0);
    png->close();
    delete png;
    if (rc != PNG_SUCCESS) {
      LOG_ERR("SLP", "PNG overlay decode failed for %s: %d", filename.c_str(), rc);
      return OverlayDrawResult::Failed;
    }
    return OverlayDrawResult::Drawn;
  };

  bool overlayDrawn = false;
  bool overlayCandidateFailed = false;
  SleepImageSelection selection;
  auto trySelectedOverlay = [&](const SleepImageSelection& image) {
    LOG_INF("SLP", "Selected overlay image: %s", image.path.c_str());
    const OverlayDrawResult result = image.isPng ? tryDrawPngOverlay(image.path) : tryDrawOverlay(image.path);
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  };

  if (selectPinnedSleepImage(SleepImageMode::Overlay, selection)) {
    trySelectedOverlay(selection);
  }
  if (!overlayDrawn && selectRandomSleepImage(SleepImageMode::Overlay, selection)) {
    trySelectedOverlay(selection);
  }

  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawOverlay("/sleep.bmp");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }
  if (!overlayDrawn) {
    const OverlayDrawResult result = tryDrawPngOverlay("/sleep.png");
    overlayDrawn = result == OverlayDrawResult::Drawn;
    overlayCandidateFailed = overlayCandidateFailed || result == OverlayDrawResult::Failed;
  }

  if (!overlayDrawn) {
    if (overlayCandidateFailed) {
      LOG_ERR("SLP", "Overlay image was found but could not be drawn; falling back to default sleep screen");
      renderer.setOrientation(savedOrientation);
      return renderDefaultSleepScreen();
    }
    LOG_DBG("SLP", "No overlay image found, displaying page without overlay");
  }

  renderer.setOrientation(savedOrientation);
  const bool shouldRunGrayscalePass =
      backgroundSupportsGrayscale && (backgroundWasRebuilt || (overlayPageBufferTrusted && !path.empty()));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, !shouldRunGrayscalePass && TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);

  if (!shouldRunGrayscalePass) {
    return;
  }

  if (!renderer.storeBwBuffer()) {
    LOG_ERR("SLP", "Overlay: failed to store BW buffer for grayscale pass");
    return;
  }

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale LSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderSavedReaderPage()) {
    LOG_ERR("SLP", "Overlay: failed to rebuild page for grayscale MSB pass");
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    return;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

void SleepActivity::onScreenshotRequest() {
  if (lastGrayscalePath.empty()) return;
  if (lastGrayscaleIsPxc) {
    if (!renderPxcSleepScreen(lastGrayscalePath)) {
      renderDefaultSleepScreen();
    }
  } else {
    FsFile file;
    if (Storage.openFileForRead("SLP", lastGrayscalePath.c_str(), file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
      }
      file.close();
    }
  }
  // Device enters deep sleep next; on wake the new activity will full-refresh anyway.
  renderer.clearScreen();
  renderer.cleanupGrayscaleWithFrameBuffer();
}
