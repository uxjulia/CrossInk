#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onFrontlightPanelClosed() override;

 private:
  void requestImageRedraw();
  void drawImage();
  void loadSiblingImages();
  bool renderPngImage();
  void doSetSleepCover();
  void confirmSetSleepCover();
  void showContextMenu();
  void promptDeleteImage();
  void unpinSleepFavorite();
  void pinBootFavorite();
  void unpinBootFavorite();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  // Set when the image must be decoded and drawn again: on entry, after changing
  // images, and after an overlay (top panel, menus, prompts) drew over it. Other
  // update requests (battery, USB) leave the image on screen as is.
  std::atomic<bool> needsImageRedraw{true};
};
