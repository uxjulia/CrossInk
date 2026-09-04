#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "activities/boot_sleep/SleepImageIndex.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isViewableImageFile(const std::string& filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
}

bool isMacOSSidecarFile(const std::string& filename) { return filename.rfind("._", 0) == 0; }

std::string imageDisplayName(const std::string& path) {
  const size_t filenameStart = path.find_last_of('/') + 1;
  const size_t extensionStart = path.find_last_of('.');
  return path.substr(filenameStart, extensionStart - filenameStart);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool isSleepFavoriteFolder(const std::string& imagePath) {
  const std::string folder = FsHelpers::extractFolderPath(imagePath);
  return equalsIgnoreCase(folder, "/sleep") || equalsIgnoreCase(folder, "/.sleep") ||
         folder == APP_STATE.preferredSleepFolderPath;
}

void drawImageError(GfxRenderer& renderer, const MappedInputManager& mappedInput, const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && !isMacOSSidecarFile(name)) {
        std::string fname(name);
        if (isViewableImageFile(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

bool BmpViewerActivity::renderPngImage() {
  ImageDimensions dims;
  if (!PngToFramebufferConverter::getDimensionsStatic(filePath, dims)) {
    drawImageError(renderer, mappedInput, "Invalid PNG File");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float scale = 1.0f;
  if (dims.width > pageWidth || dims.height > pageHeight) {
    const float scaleX = static_cast<float>(pageWidth) / static_cast<float>(dims.width);
    const float scaleY = static_cast<float>(pageHeight) / static_cast<float>(dims.height);
    scale = std::min(scaleX, scaleY);
  }

  const int drawWidth = std::max(1, static_cast<int>(static_cast<float>(dims.width) * scale));
  const int drawHeight = std::max(1, static_cast<int>(static_cast<float>(dims.height) * scale));
  const int x = (pageWidth - drawWidth) / 2;
  const int y = (pageHeight - drawHeight) / 2;

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = drawWidth;
  config.maxHeight = drawHeight;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;

  PngToFramebufferConverter converter;
  renderer.clearScreen();
  if (!converter.decodeToFramebuffer(filePath, renderer, config)) {
    drawImageError(renderer, mappedInput, "Invalid PNG File");
    return false;
  }
  renderer.preserveImagePolarity(x, y, drawWidth, drawHeight);

  bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                  currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SET_SLEEP_COVER),
                                            (hasPrevious ? "<" : nullptr), (hasNext ? ">" : nullptr));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  if (FsHelpers::hasPngExtension(filePath)) {
    renderPngImage();
    return;
  }

  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SET_SLEEP_COVER),
                                                (hasPrevious ? "<" : nullptr), (hasNext ? ">" : nullptr));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    } else {
      // Handle file parsing error
      drawImageError(renderer, mappedInput, "Invalid BMP File");
    }

    file.close();
  } else {
    // Handle file open error
    drawImageError(renderer, mappedInput, "Could not open file");
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  APP_STATE.favoriteSleepImagePath = filePath;
  if (APP_STATE.saveToFile()) {
    LOG_INF("BmpViewer", "Pinned favorite sleep image: %s", filePath.c_str());

    // PNG covers only render in Page Overlay sleep mode; other modes silently skip them (see
    // selectPinnedSleepImage() in SleepActivity.cpp), so switch the setting for the user.
    if (FsHelpers::hasPngExtension(filePath) &&
        SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY;
      if (SETTINGS.saveToFile()) {
        LOG_INF("BmpViewer", "Switched sleep wallpaper mode to Page Overlay for PNG cover");
      } else {
        LOG_ERR("BmpViewer", "Failed to save sleep wallpaper mode after setting PNG cover");
      }
    }

    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    LOG_ERR("BmpViewer", "Failed to save favorite sleep image path: %s", filePath.c_str());
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::pinSleepFavorite() {
  APP_STATE.favoriteSleepImagePath = filePath;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to save favorite sleep image path: %s", filePath.c_str());
    return;
  }
  LOG_INF("BmpViewer", "Pinned favorite sleep image: %s", filePath.c_str());
}

void BmpViewerActivity::unpinSleepFavorite() {
  APP_STATE.favoriteSleepImagePath.clear();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to clear favorite sleep image");
    return;
  }
  LOG_INF("BmpViewer", "Cleared favorite sleep image");
}

void BmpViewerActivity::promptDeleteImage() {
  const std::string path = filePath;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE),
                                             imageDisplayName(path)),
      [this, path](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (!Storage.remove(path.c_str())) {
          LOG_ERR("BmpViewer", "Failed to delete image: %s", path.c_str());
          return;
        }
        SleepImageIndex::invalidateForPath(path.c_str());
        if (APP_STATE.favoriteSleepImagePath == path) {
          unpinSleepFavorite();
        }
        activityManager.goToFileBrowser(path);
      });
}

void BmpViewerActivity::showContextMenu() {
  std::vector<FileBrowserActionActivity::MenuItem> items = BookActions::buildBookActionItems(filePath, false);
  if (BookActions::canSendNearby(filePath)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  if (isSleepFavoriteFolder(filePath)) {
    const bool isPinned = APP_STATE.favoriteSleepImagePath == filePath;
    items.push_back({isPinned ? FileBrowserAction::UnpinFavorite : FileBrowserAction::PinFavorite,
                     isPinned ? StrId::STR_UNPIN_AS_FAVORITE : StrId::STR_PIN_AS_FAVORITE});
  }

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, imageDisplayName(filePath),
                                                                     std::move(items), false, false),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (actionResult == nullptr) return;

                           switch (static_cast<FileBrowserAction>(actionResult->action)) {
                             case FileBrowserAction::Delete:
                               promptDeleteImage();
                               return;
                             case FileBrowserAction::SendNearby:
                               activityManager.goToNearbyBookSend(filePath, false);
                               return;
                             case FileBrowserAction::PinFavorite:
                               if (FsHelpers::hasPngExtension(filePath)) {
                                 startActivityForResult(std::make_unique<ConfirmationActivity>(
                                                            renderer, mappedInput, "", tr(STR_PIN_PNG_WARNING)),
                                                        [this](const ActivityResult& confirmation) {
                                                          if (!confirmation.isCancelled) pinSleepFavorite();
                                                        });
                               } else {
                                 pinSleepFavorite();
                               }
                               return;
                             case FileBrowserAction::UnpinFavorite:
                               unpinSleepFavorite();
                               return;
                             case FileBrowserAction::DeleteCache:
                             case FileBrowserAction::SetSleepFolder:
                             case FileBrowserAction::ClearSleepFolder:
                             case FileBrowserAction::ToggleCompleted:
                             case FileBrowserAction::RemoveFromRecents:
                             case FileBrowserAction::DeleteStats:
                             case FileBrowserAction::ViewBookmarks:
                             case FileBrowserAction::ViewClippings:
                             case FileBrowserAction::DeleteBookmarks:
                             case FileBrowserAction::DeleteClippings:
                             case FileBrowserAction::EpubRenderMode:
                             case FileBrowserAction::ResetReaderSettings:
                               return;
                           }
                         });
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.hasTouchHardware()) {
    constexpr unsigned long CONTEXT_MENU_HOLD_MS = 1000;
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.isScreenTouchLongPress(touchX, touchY, CONTEXT_MENU_HOLD_MS)) {
      mappedInput.suppressCurrentTouchContact();
      showContextMenu();
      return;
    }
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      showContextMenu();
      return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isViewableImageFile(filePath)) {
      doSetSleepCover();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
