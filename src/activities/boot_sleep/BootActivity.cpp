#include "BootActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AppVersion.h"
#include "CrossPointState.h"
#include "ImageFolderIndex.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {

// A malformed/hostile BMP header can otherwise report implausible
// dimensions; this is well beyond any real panel and only guards the draw
// math below, not a supported image size.
constexpr int MAX_BOOT_IMAGE_DIMENSION = 4000;

bool isBootImagePath(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

// Letterboxed/centered draw matching BmpViewerActivity's simple centering
// (no crop or invert options — boot has no settings surface of its own).
void drawCenteredBootBitmap(const GfxRenderer& renderer, const Bitmap& bitmap) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int x, y;

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      x = 0;
      y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2));
    } else {
      x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2));
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
}

bool tryDrawCustomBootImage(const GfxRenderer& renderer, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("BOOT", path, file)) {
    LOG_ERR("BOOT", "Failed to open custom boot image: %s", path.c_str());
    return false;
  }

  Bitmap bitmap(file, true);
  const BmpReaderError parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("BOOT", "Failed to parse custom boot BMP %s: %s", path.c_str(), Bitmap::errorToString(parseResult));
    file.close();
    return false;
  }
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || bitmap.getWidth() > MAX_BOOT_IMAGE_DIMENSION ||
      bitmap.getHeight() > MAX_BOOT_IMAGE_DIMENSION) {
    LOG_ERR("BOOT", "Custom boot BMP %s has implausible dimensions %dx%d", path.c_str(), bitmap.getWidth(),
            bitmap.getHeight());
    file.close();
    return false;
  }

  drawCenteredBootBitmap(renderer, bitmap);
  file.close();
  return true;
}

bool tryDrawPinnedBootImage(const GfxRenderer& renderer) {
  const std::string& favorite = APP_STATE.favoriteBootImagePath;
  if (favorite.empty() || !isBootImagePath(favorite) || !Storage.exists(favorite.c_str())) return false;

  LOG_INF("BOOT", "Loading pinned boot image: %s", favorite.c_str());
  return tryDrawCustomBootImage(renderer, favorite);
}

bool tryDrawRotatingBootImage(const GfxRenderer& renderer) {
  std::string bootDir;
  if (!ImageFolderIndex::resolveBootScreenDirectory(bootDir)) return false;

  const auto pickAndDraw = [&](const bool validateBmpHeaders) {
    ImageFolderIndex::Selection selection;
    if (!ImageFolderIndex::select(bootDir, false, validateBmpHeaders, APP_STATE.recentBootImages,
                                  CrossPointState::BOOT_RECENT_COUNT, APP_STATE.recentBootPos, APP_STATE.recentBootFill,
                                  std::min(APP_STATE.recentBootFill, CrossPointState::BOOT_RECENT_COUNT), selection)) {
      return false;
    }
    if (!tryDrawCustomBootImage(renderer, selection.path)) return false;

    APP_STATE.pushRecentBoot(selection.index);
    APP_STATE.saveToFile();
    return true;
  };

  if (pickAndDraw(false)) return true;

  // A corrupt BMP shouldn't strand an otherwise valid folder on the fallback
  // logo forever; re-scan with header validation once, same recovery sleep
  // screens use for their own folder.
  return pickAndDraw(true);
}

void drawDefaultBootLogo(const GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSINK), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSINK_VERSION);
}

}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  if (!tryDrawPinnedBootImage(renderer) && !tryDrawRotatingBootImage(renderer)) {
    drawDefaultBootLogo(renderer);
  }

  renderer.displayBuffer();
}
