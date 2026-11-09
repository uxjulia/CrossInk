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

bool shouldPrepareFullCover() {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM ||
         SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_OVERLAY;
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

bool prepareXtc(const Xtc& xtc) {
  bool success = true;
  if (shouldPrepareFullCover()) {
    success = xtc.generateCoverBmp() && success;
  }
  // Minimal and Dashboard thumbnails depend on the final sleep orientation and
  // calibrated bounds. SleepActivity prepares them after selecting that orientation.
  return success;
}

bool prepareTxt(const Txt& txt) {
  if (!shouldPrepareFullCover() && !shouldPrepareMinimalCover() && !shouldPrepareDashboardCover()) {
    return true;
  }
  return txt.generateCoverBmp();
}

bool prepareFullCoverForPath(const std::string& bookPath, const bool cropped, const GfxRenderer* renderer,
                             ImageToneProfile toneProfile) {
  if (bookPath.empty()) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateCoverBmp(cropped, renderer, readerFontIdForRenderer(renderer), toneProfile);
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
    return txt.generateCoverBmp(toneProfile);
  }
  return false;
}

bool prepareMinimalCoverForPath(const std::string& bookPath, const GfxRenderer& renderer, const bool statsSleep) {
  const auto coverRect = MinimalTheme::getSleepCoverRect(renderer, statsSleep);
  if (bookPath.empty() || coverRect.width <= 0 || coverRect.height <= 0) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateAdaptiveThumbBmp(coverRect.width, coverRect.height, &renderer,
                                         readerFontIdForRenderer(&renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateThumbBmp(static_cast<uint16_t>(coverRect.width), static_cast<uint16_t>(coverRect.height));
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateCoverBmp();
  }
  return false;
}

bool prepareDashboardCoverForPath(const std::string& bookPath, const GfxRenderer& renderer) {
  const auto coverRect = DashboardTheme::getSleepCoverRect(renderer);
  if (bookPath.empty() || coverRect.width <= 0 || coverRect.height <= 0) {
    return false;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      return false;
    }
    return epub.generateAdaptiveThumbBmp(coverRect.width, coverRect.height, &renderer,
                                         readerFontIdForRenderer(&renderer));
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) {
      return false;
    }
    return xtc.generateThumbBmp(static_cast<uint16_t>(coverRect.width), static_cast<uint16_t>(coverRect.height));
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

std::string cachedCoverPathFor(const std::string& bookPath, const bool cropped, ImageToneProfile toneProfile,
                               bool* sourceBitmap) {
  if (sourceBitmap) *sourceBitmap = false;
  std::string coverPath;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    coverPath = Epub(bookPath, "/.crosspoint").getCoverBmpPath(cropped, toneProfile);
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    if (sourceBitmap) *sourceBitmap = true;
    coverPath = Xtc(bookPath, "/.crosspoint").getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    coverPath = Txt(bookPath, "/.crosspoint").getCoverBmpPath(toneProfile, sourceBitmap);
  }

  return fileExists(coverPath) ? coverPath : std::string{};
}

std::string cachedMinimalCoverPathFor(const std::string& bookPath, const GfxRenderer& renderer, const bool statsSleep) {
  const auto coverRect = MinimalTheme::getSleepCoverRect(renderer, statsSleep);
  if (coverRect.width <= 0 || coverRect.height <= 0) return {};
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(coverRect.width, coverRect.height);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath = UITheme::getCoverThumbPath(reusablePath, coverRect.width, coverRect.height, false);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

std::string cachedDashboardCoverPathFor(const std::string& bookPath, const GfxRenderer& renderer) {
  const auto coverRect = DashboardTheme::getSleepCoverRect(renderer);
  if (coverRect.width <= 0 || coverRect.height <= 0) return {};
  if (FsHelpers::hasEpubExtension(bookPath)) {
    const Epub epub(bookPath, "/.crosspoint");
    const std::string coverPath = epub.getAdaptiveThumbBmpPath(coverRect.width, coverRect.height);
    return fileExists(coverPath) ? epub.getThumbBmpPath() : std::string{};
  }

  const std::string reusablePath = reusableCoverPathFor(bookPath);
  const std::string coverPath = UITheme::getCoverThumbPath(reusablePath, coverRect.width, coverRect.height, false);
  return fileExists(coverPath) ? reusablePath : std::string{};
}

}  // namespace SleepCoverAssets
