#include "SleepActivity.h"

#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PNGdec.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <string_view>

#include "../home/RecentBookProgress.h"
#include "../reader/BookStatsView.h"
#include "../reader/EpubReaderActivity.h"
#include "../reader/EpubReaderUtils.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "AppCapabilities.h"
#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ImageFolderIndex.h"
#include "RecentBooksStore.h"
#include "SleepCoverAssets.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {

constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
constexpr int sleepBuildInfoSideMargin = 20;

bool sleepCoverFilterInvertsGeneratedScreen() {
  return SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE;
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

RecentBook recentBookForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto book =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& candidate) { return candidate.path == path; });
  if (book != books.end()) {
    return *book;
  }

  RecentBook loadedBook = RECENT_BOOKS.getDataFromBook(path);
  if (loadedBook.title.empty()) {
    loadedBook.title = filenameFromPath(path);
  }
  return loadedBook;
}

std::string bookStatsCachePathFor(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub::cachePathForFilePath(path, "/.crosspoint");
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return {};
}

BookReadingStats loadBookStatsForPath(const std::string& path) {
  const std::string cachePath = bookStatsCachePathFor(path);
  if (cachePath.empty()) {
    return BookReadingStats{};
  }
  return BookReadingStats::load(cachePath);
}

std::string loadChapterTitleForPath(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) {
    return {};
  }

  Epub epub(path, "/.crosspoint");
  if (!epub.load(false, true, Epub::XLocationLoadMode::Skip)) {
    return {};
  }

  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgress(epub, progress, "SLP")) {
    return {};
  }

  const auto spineItem = epub.getSpineItem(progress.spineIndex);
  if (spineItem.tocIndex < 0) {
    return {};
  }

  const auto tocItem = epub.getTocItem(spineItem.tocIndex);
  return tocItem.title;
}

enum class OverlayDrawResult : uint8_t { NotFound, Drawn, Failed };

enum class SleepImageMode : uint8_t { Custom, Overlay };

struct SleepImageSelection {
  std::string path;
  bool isPng = false;
};

bool isBmpSleepImagePath(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool isPngSleepImagePath(const std::string& path) { return FsHelpers::hasPngExtension(path); }

bool tryOpenSleepDirectory(FsFile& dir, std::string& sleepDir, const std::string& candidate) {
  if (candidate.empty()) {
    return false;
  }

  dir = Storage.open(candidate.c_str());
  if (dir && dir.isDirectory()) {
    sleepDir = candidate;
    return true;
  }

  if (dir) {
    dir.close();
  }
  return false;
}

bool resolvePreferredSleepDirectory(std::string& sleepDir) {
  sleepDir.clear();

  const auto folderExists = [&sleepDir](const std::string& candidate) {
    if (candidate.empty() || !Storage.exists(candidate.c_str())) return false;
    sleepDir = candidate;
    return true;
  };

  if (folderExists(APP_STATE.preferredSleepFolderPath)) return true;

  if (!APP_STATE.preferredSleepFolderPath.empty()) {
    LOG_INF("SLP", "Preferred sleep folder missing, falling back: %s", APP_STATE.preferredSleepFolderPath.c_str());
  }

  char defaultSleepDir[16];
  if (FsHelpers::resolveRootDirectoryIgnoreCase("/.sleep", defaultSleepDir, sizeof(defaultSleepDir)) &&
      folderExists(defaultSleepDir)) {
    return true;
  }
  return FsHelpers::resolveRootDirectoryIgnoreCase("/sleep", defaultSleepDir, sizeof(defaultSleepDir)) &&
         folderExists(defaultSleepDir);
}

bool openPreferredSleepDirectory(FsFile& dir, std::string& sleepDir) {
  if (!resolvePreferredSleepDirectory(sleepDir)) return false;

  return tryOpenSleepDirectory(dir, sleepDir, sleepDir);
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
    return true;
  }

  if (isPngSleepImagePath(favorite)) {
    if (mode == SleepImageMode::Overlay) {
      selection.path = favorite;
      selection.isPng = true;
      return true;
    }

    LOG_INF("SLP", "Pinned PNG sleep image requires Page Overlay mode, falling back: %s", favorite.c_str());
    return false;
  }

  LOG_ERR("SLP", "Pinned sleep image has unsupported extension: %s", favorite.c_str());
  return false;
}

bool selectRandomSleepImage(SleepImageMode mode, SleepImageSelection& selection, bool validateBmpHeaders = false,
                            bool bmpOnly = false) {
  FsFile dir;
  std::string sleepDir;
  if (!resolvePreferredSleepDirectory(sleepDir)) return false;

  const bool allowPng = mode == SleepImageMode::Overlay && !bmpOnly;
  ImageFolderIndex::Selection indexedSelection;
  if (ImageFolderIndex::select(sleepDir, allowPng, validateBmpHeaders, APP_STATE.recentSleepImages,
                               CrossPointState::SLEEP_RECENT_COUNT, APP_STATE.recentSleepPos, APP_STATE.recentSleepFill,
                               std::min(APP_STATE.recentSleepFill, CrossPointState::SLEEP_RECENT_COUNT),
                               indexedSelection)) {
    selection.path = std::move(indexedSelection.path);
    selection.isPng = indexedSelection.isPng;
    APP_STATE.pushRecentSleep(indexedSelection.index);
    APP_STATE.saveToFile();
    return true;
  }

  // Cache creation is best-effort. Reopen the directory only for the legacy
  // reservoir fallback when the cache could not be loaded or written.
  if (!openPreferredSleepDirectory(dir, sleepDir)) return false;

  // Keep one reservoir for every candidate and one that excludes recent images.
  // This avoids holding the whole directory in RAM or opening every BMP just to
  // parse its header before picking one.
  std::string nonRecentPath;
  uint16_t candidateCount = 0;
  uint16_t selectedIndex = 0;
  uint16_t nonRecentCount = 0;
  uint16_t nonRecentIndex = 0;
  const uint8_t recentWindow = std::min(APP_STATE.recentSleepFill, CrossPointState::SLEEP_RECENT_COUNT);
  const auto setSleepImagePath = [&](std::string& path, std::string_view filename) {
    path = sleepDir;
    path += '/';
    path.append(filename.data(), filename.size());
  };
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    const std::string_view filename(name);
    if (filename.empty() || filename.front() == '.') {
      file.close();
      continue;
    }

    const bool isBmp = FsHelpers::hasBmpExtension(filename);
    const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
    if (!isBmp && !isPng) {
      file.close();
      continue;
    }

    // The normal path defers this SD read until after selection so folders with
    // many wallpapers stay fast. Only use the slower validation pass after a
    // selected BMP failed to render.
    if (isBmp && validateBmpHeaders) {
      Bitmap bitmap(file);
      const BmpReaderError parseResult = bitmap.parseHeaders();
      if (parseResult != BmpReaderError::Ok) {
        LOG_ERR("SLP", "Skipping invalid BMP sleep image %s/%.*s: %s", sleepDir.c_str(),
                static_cast<int>(filename.size()), filename.data(), Bitmap::errorToString(parseResult));
        file.close();
        continue;
      }
    }

    if (candidateCount == UINT16_MAX) {
      file.close();
      continue;
    }

    candidateCount++;
    const uint16_t candidateIndex = candidateCount - 1;
    if (random(candidateCount) == 0) {
      setSleepImagePath(selection.path, filename);
      selectedIndex = candidateIndex;
    }

    if (!APP_STATE.isRecentSleep(candidateIndex, recentWindow)) {
      nonRecentCount++;
      if (random(nonRecentCount) == 0) {
        setSleepImagePath(nonRecentPath, filename);
        nonRecentIndex = candidateIndex;
      }
    }
    file.close();
  }
  dir.close();

  if (candidateCount == 0) {
    return false;
  }

  if (nonRecentCount > 0) {
    selection.path = std::move(nonRecentPath);
    selectedIndex = nonRecentIndex;
  }

  // With fewer images than the recent-history window, every candidate can be
  // recent. Fall back to the all-candidates reservoir so a custom sleep screen
  // still renders.
  APP_STATE.pushRecentSleep(selectedIndex);
  APP_STATE.saveToFile();
  selection.isPng = FsHelpers::hasPngExtension(selection.path);
  return true;
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();
  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  // Sleep screens draw directly, outside ActivityManager's normal render path.
  // Quick Resume retains the current screen, so preserve its Night Mode output;
  // generated sleep screens continue to use their normal polarity.
  display.setInverted(renderQuickResume && SETTINGS.screenInverted != 0);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  const auto sleepScreen = SETTINGS.sleepScreen;
  const bool sleepScreenUsesRecentBooks = sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::READING_STATS_SLEEP ||
                                          sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP ||
                                          sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP ||
                                          sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::DASHBOARD_SLEEP;
  const std::string& recentBookPath = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (sleepScreenUsesRecentBooks && !recentBookPath.empty()) {
    RECENT_BOOKS.ensureLoaded();
  }

  overlayBackgroundBufferStored =
      sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY && renderer.storeBwBuffer();

  // X4 Pro and X4 Classic share a panel that can retain this high-contrast
  // transient update beneath the final OEM-style sleep refresh. Render only
  // the final sleep frame on that panel family.
  const bool showSleepPopup = !BoardConfig::isX4Pro() && !CROSSINK_APP_DEVICE_X4CLASSIC;
  // Show the popup in the orientation that was visible before reader exit restores
  // global settings. Reset to portrait afterwards so sleep screen layout stays unchanged.
  if (APP_STATE.lastSleepFromReader) {
    if (showSleepPopup) {
      renderer.setOrientation(sleepPopupOrientation);
      GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    }
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else if (showSleepPopup) {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (sleepScreen) {
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
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP):
      return renderMinimalSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP):
      return renderMinimalStatsSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::DASHBOARD_SLEEP):
      return renderDashboardSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  const auto tryRenderSelection = [this](const SleepImageSelection& selection) {
    FsFile file;
    if (!Storage.openFileForRead("SLP", selection.path, file)) {
      LOG_ERR("SLP", "Failed to open custom sleep image: %s", selection.path.c_str());
      return false;
    }

    LOG_INF("SLP", "Loading custom sleep image: %s", selection.path.c_str());
    delay(100);
    // Dither grayscale custom sleep images so their tonal detail survives the
    // 1-bit sleep-screen render.
    Bitmap bitmap(file, true);
    const BmpReaderError parseResult = bitmap.parseHeaders();
    if (parseResult != BmpReaderError::Ok) {
      LOG_ERR("SLP", "Failed to parse custom sleep BMP %s: %s", selection.path.c_str(),
              Bitmap::errorToString(parseResult));
      return false;
    }

    renderBitmapSleepScreen(bitmap);
    return true;
  };

  SleepImageSelection selection;
  if (selectPinnedSleepImage(SleepImageMode::Custom, selection) && tryRenderSelection(selection)) {
    return;
  }

  if (selectRandomSleepImage(SleepImageMode::Custom, selection) && tryRenderSelection(selection)) {
    return;
  }

  // A corrupt BMP should not make an otherwise valid custom folder fall back
  // to the dark default screen. Re-scan only on this error path and choose
  // from the files whose headers are valid.
  if (!selection.path.empty() && selectRandomSleepImage(SleepImageMode::Custom, selection, true) &&
      tryRenderSelection(selection)) {
    return;
  }

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSINK), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  const bool lightSleepScreen = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT;
  if (!lightSleepScreen) {
    renderer.invertScreen();
  }

#ifdef CROSSINK_SHOW_SLEEP_BUILD_INFO
  const std::string buildInfo = std::string(CROSSINK_BUILD_ENV) + " " + CROSSINK_VERSION;
  const std::string visibleBuildInfo =
      renderer.truncatedText(SMALL_FONT_ID, buildInfo.c_str(), pageWidth - sleepBuildInfoSideMargin * 2);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 118, visibleBuildInfo.c_str(), lightSleepScreen);
#endif

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  // Keep error diffusion on the screen-sized grid. Resampling an already
  // dithered source makes the source pattern alias into regular seams.
  if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::FIT &&
      (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight)) {
    const float scale = std::min(static_cast<float>(pageWidth) / bitmap.getWidth(),
                                 static_cast<float>(pageHeight) / bitmap.getHeight());
    const int targetWidth = static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1;
    const int targetHeight = static_cast<int>(std::floor((bitmap.getHeight() - 1) * scale)) + 1;
    bitmap.setDitheredOutputSize(targetWidth, targetHeight);
  }

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    renderer.setRenderMode(GfxRenderer::BW);
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

  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  std::string coverBmpPath = SleepCoverAssets::cachedCoverPathFor(path, cropped);
  if (coverBmpPath.empty() && SleepCoverAssets::prepareFullCoverForPath(path, cropped, &renderer)) {
    coverBmpPath = SleepCoverAssets::cachedCoverPathFor(path, cropped);
  }
  if (coverBmpPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderReadingStatsSleepScreen() const {
  BookReadingStats bookStats;
  std::string bookTitle = tr(STR_READING_STATS);
  float progressPercent = -1.0f;

  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (!path.empty()) {
    const std::string recentTitle = recentTitleForPath(path);
    bookTitle = recentTitle.empty() ? filenameFromPath(path) : recentTitle;

    bookStats = loadBookStatsForPath(path);
    progressPercent = RecentBookProgress::loadPercent(recentBookForPath(path));
  }

  if (!halClock.isAvailable()) {
    const GlobalReadingStats deviceStats = GlobalReadingStats::load();
    const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
    const GlobalReadingStats allDevicesStats =
        hasSyncedStats ? GlobalReadingStats::loadAggregated(deviceStats) : GlobalReadingStats{};
    renderNoRtcCombinedStatsPage(renderer, nullptr, bookTitle, bookStats, progressPercent, false, 0, deviceStats,
                                 hasSyncedStats ? &allDevicesStats : nullptr, false);
  } else {
    renderPerBookStatsPage(renderer, nullptr, bookTitle, bookStats, progressPercent, false, 0, false, false, false);
  }
  if (!sleepCoverFilterInvertsGeneratedScreen()) {
    renderer.invertScreen();
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderMinimalSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);
  if (book.coverBmpPath.empty() && SleepCoverAssets::prepareMinimalCoverForPath(path, &renderer)) {
    book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);
  }

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const float progressPercent = RecentBookProgress::loadPercent(book);
  MinimalTheme theme;
  theme.drawSleepScreen(renderer, book, &bookStats, progressPercent, sleepCoverFilterInvertsGeneratedScreen());
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderMinimalStatsSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);
  if (book.coverBmpPath.empty() && SleepCoverAssets::prepareMinimalCoverForPath(path, &renderer)) {
    book.coverBmpPath = SleepCoverAssets::cachedMinimalCoverPathFor(path);
  }

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const GlobalReadingStats globalStats = GlobalReadingStats::load();
  const float progressPercent = RecentBookProgress::loadPercent(book);
  MinimalTheme theme;
  theme.drawStatsSleepScreen(renderer, book, &bookStats, &globalStats, progressPercent,
                             sleepCoverFilterInvertsGeneratedScreen());
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderDashboardSleepScreen() const {
  const std::string& path = currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath;
  if (path.empty()) {
    return renderDefaultSleepScreen();
  }

  RecentBook book = recentBookForPath(path);
  const std::string fallbackCoverPath = book.coverBmpPath;
  book.coverBmpPath = SleepCoverAssets::cachedDashboardCoverPathFor(path);
  if (book.coverBmpPath.empty() && SleepCoverAssets::prepareDashboardCoverForPath(path, &renderer)) {
    book.coverBmpPath = SleepCoverAssets::cachedDashboardCoverPathFor(path);
  }
  if (book.coverBmpPath.empty()) {
    book.coverBmpPath = fallbackCoverPath;
  }

  const BookReadingStats bookStats = loadBookStatsForPath(path);
  const GlobalReadingStats globalStats = GlobalReadingStats::load();
  const float progressPercent = RecentBookProgress::loadPercent(book);
  const std::string chapterTitle = loadChapterTitleForPath(path);
  DashboardTheme theme;
  theme.drawSleepScreen(renderer, book, &bookStats, &globalStats, progressPercent, chapterTitle.c_str(),
                        sleepCoverFilterInvertsGeneratedScreen());
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  if (ReaderUtils::readerDarkModeEnabled()) {
    renderer.drawImageInverted(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  } else {
    renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  }
  if (gpio.deviceIsX3()) {
    // The controller still holds the displayed page, so its differential base
    // waveform can add the moon without a full-screen flash.
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
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
  const bool shouldUseReaderPageBackground = canSnapshotOverlayBackground;
  const std::string path = shouldUseReaderPageBackground
                               ? (currentBookPath.empty() ? APP_STATE.openEpubPath : currentBookPath)
                               : std::string{};

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
  bool backgroundAvailable = false;

  // Step 1: Restore the screen that was visible before the sleep popup. When
  // that snapshot is unavailable in the reader, rebuild from the saved position.
  if (overlayBackgroundBufferStored) {
    renderer.restoreBwBuffer();
    backgroundAvailable = true;
  } else if (shouldUseReaderPageBackground && !path.empty()) {
    backgroundWasRebuilt = renderSavedReaderPage();
    backgroundAvailable = backgroundWasRebuilt;

    if (!backgroundWasRebuilt) {
      LOG_DBG("SLP", "Page re-render failed, using white background");
      renderer.clearScreen();
    }
  } else {
    LOG_DBG("SLP", "No current screen snapshot available for overlay sleep screen");
    renderer.clearScreen();
  }

  // Remove the live battery strip from the preserved/reconstructed reader page so the
  // overlay sleep screen still shows chapter/progress details without the battery glance target.
  if (shouldUseReaderPageBackground && backgroundAvailable) {
    hideOverlayBatteryStrip(renderer);
  }

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
    // Keep dithering off here: error diffusion can make nominally white
    // transparent pixels visible over the preserved reader page.
    Bitmap bitmap(file);
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

    // The reader activity has already released its document/layout state, and
    // Page Overlay has captured the page framebuffer. Its active SD-font glyph
    // cache is therefore regenerable and not needed for the final sleep frame.
    // Free it before PNGdec requests its contiguous decode buffer.
    const uint32_t freeBeforeRelease = ESP.getFreeHeap();
    const uint32_t maxAllocBeforeRelease = ESP.getMaxAllocHeap();
    if (renderer.releaseSdCardFontForLowMemory(SETTINGS.getReaderFontId())) {
      LOG_DBG("SLP", "Released reader font cache for PNG overlay: free=%u->%u maxAlloc=%u->%u", freeBeforeRelease,
              ESP.getFreeHeap(), maxAllocBeforeRelease, ESP.getMaxAllocHeap());
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

  // Page Overlay can mix PNGs and BMPs. PNG decoding needs a sizeable
  // temporary buffer while the reader page is still resident, so an otherwise
  // valid PNG can fail on C3 devices. Try another folder image before using
  // the root/default fallback: use BMP-only after a PNG failure, or validate
  // BMP headers after a failed BMP selection.
  if (!overlayDrawn && overlayCandidateFailed && !selection.path.empty() &&
      selectRandomSleepImage(SleepImageMode::Overlay, selection,
                             /*validateBmpHeaders=*/!selection.isPng,
                             /*bmpOnly=*/selection.isPng)) {
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
    if (!backgroundAvailable) {
      LOG_DBG("SLP", "No overlay image or current screen snapshot available, falling back to default sleep screen");
      renderer.setOrientation(savedOrientation);
      return renderDefaultSleepScreen();
    }
    LOG_DBG("SLP", "No overlay image found, displaying background without overlay");
  }

  renderer.setOrientation(savedOrientation);
  // The grayscale re-render has no mask for the overlay image. If an overlay was
  // drawn, keep the composited BW frame intact instead of painting page glyphs
  // over the sleep image.
  const bool shouldRunGrayscalePass = shouldUseReaderPageBackground && backgroundSupportsGrayscale && !overlayDrawn &&
                                      (backgroundWasRebuilt || (overlayBackgroundBufferStored && !path.empty()));
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
