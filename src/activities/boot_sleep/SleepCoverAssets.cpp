#include "SleepCoverAssets.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/minimal/MinimalTheme.h"

namespace {

constexpr int kMinimalSleepCoverHeight = MinimalMetrics::homeCoverImageHeight;
constexpr int kMinimalSleepCoverWidth = MinimalMetrics::homeCoverImageWidth;
constexpr int kDashboardSleepCoverHeight = DashboardMetrics::homeCoverImageHeight;
constexpr int kDashboardSleepCoverWidth = DashboardMetrics::homeCoverImageWidth;

bool shouldPrepareFullCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM;
}

bool shouldPrepareMinimalCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_SLEEP ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::MINIMAL_STATS_SLEEP;
}

bool shouldPrepareDashboardCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::DASHBOARD_SLEEP;
}

bool fileExists(const std::string& path) { return !path.empty() && Storage.exists(path.c_str()); }

int readerFontIdForRenderer(const GfxRenderer* renderer) { return renderer ? SETTINGS.getReaderFontId() : 0; }

}  // namespace

namespace SleepCoverAssets {

bool prepareEpub(const Epub& epub, const GfxRenderer* renderer) {
  bool success = true;
  const int readerFontId = readerFontIdForRenderer(renderer);
  if (shouldPrepareFullCover()) {
    const bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
    success = epub.generateCoverBmp(cropped, renderer, readerFontId) && success;
  }
  if (shouldPrepareMinimalCover()) {
    success =
        epub.generateAdaptiveThumbBmp(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight, renderer, readerFontId) &&
        success;
  }
  if (shouldPrepareDashboardCover()) {
    success =
        epub.generateAdaptiveThumbBmp(kDashboardSleepCoverWidth, kDashboardSleepCoverHeight, renderer, readerFontId) &&
        success;
  }
  return success;
}

bool prepareXtc(const Xtc& xtc) {
  bool success = true;
  if (shouldPrepareFullCover()) {
    success = xtc.generateCoverBmp() && success;
  }
  if (shouldPrepareMinimalCover()) {
    success = xtc.generateThumbBmp(static_cast<uint16_t>(kMinimalSleepCoverWidth),
                                   static_cast<uint16_t>(kMinimalSleepCoverHeight)) &&
              success;
  }
  if (shouldPrepareDashboardCover()) {
    success = xtc.generateThumbBmp(static_cast<uint16_t>(kDashboardSleepCoverWidth),
                                   static_cast<uint16_t>(kDashboardSleepCoverHeight)) &&
              success;
  }
  return success;
}

bool prepareTxt(const Txt& txt) {
  if (!shouldPrepareFullCover() && !shouldPrepareMinimalCover() && !shouldPrepareDashboardCover()) {
    return true;
  }
  return txt.generateCoverBmp();
}

bool prepareFullCoverForPath(const std::string& bookPath, const bool cropped, const GfxRenderer* renderer) {
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
      return false;
    }
    return epub.generateCoverBmp(cropped, renderer, readerFontIdForRenderer(renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateCoverBmp();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp();
  }
  return false;
}

bool prepareDashboardCoverForPath(const std::string& bookPath, const GfxRenderer* renderer) {
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
      return false;
    }
    return epub.generateAdaptiveThumbBmp(kDashboardSleepCoverWidth, kDashboardSleepCoverHeight, renderer,
                                         readerFontIdForRenderer(renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateThumbBmp(static_cast<uint16_t>(kDashboardSleepCoverWidth),
                                static_cast<uint16_t>(kDashboardSleepCoverHeight));
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp();
  }
  return false;
}

std::string reusableCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    return Epub(bookPath, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    return Xtc(bookPath, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    return Txt(bookPath, "/.crosspoint").getCoverBmpPath();
  }
  return {};
}

std::string cachedCoverPathFor(const std::string& bookPath, const bool cropped) {
  std::string coverPath;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    coverPath = Epub(bookPath, "/.crosspoint").getCoverBmpPath(cropped);
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    coverPath = Xtc(bookPath, "/.crosspoint").getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    coverPath = Txt(bookPath, "/.crosspoint").getCoverBmpPath();
  }

  return fileExists(coverPath) ? coverPath : std::string{};
}

std::string cachedMinimalCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath =
      UITheme::getCoverThumbPath(reusablePath, kMinimalSleepCoverWidth, kMinimalSleepCoverHeight);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

std::string cachedDashboardCoverPathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(kDashboardSleepCoverWidth, kDashboardSleepCoverHeight);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath =
      UITheme::getCoverThumbPath(reusablePath, kDashboardSleepCoverWidth, kDashboardSleepCoverHeight);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

}  // namespace SleepCoverAssets
