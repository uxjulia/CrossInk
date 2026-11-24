#pragma once
#include <string>
#include <utility>

#include "ChargingBookPreparation.h"
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false,
                         GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait,
                         bool preserveSleepFrame = false)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout),
        sleepPopupOrientation(sleepPopupOrientation),
        preserveSleepFrame(preserveSleepFrame) {}
  void onEnter() override;
  void showSleepPopup();
  void paintSleepScreen();
  void setChargingPreparation(ChargingBookPreparation* job) { preparation = job; }
  void discardSavedBackground() {
    quickResumeBackgroundAvailable = false;
    overlayBackgroundAvailable = false;
  }
  void render(RenderLock&&) override {
    if (preparation) preparation->render(renderer, preserveSleepFrame ? nullptr : this);
  }
  // Whether this screen is independent of the outgoing activity's saves.
  bool rendersBeforeExit() const;
  bool hasMonochromeFrame() const { return monochromeFrame; }

 private:
#ifdef SIMULATOR
  friend struct UpstreamFeatureSmokeTest;
#endif
  mutable std::string preparationArtwork;          // Reuse a shuffled image for the final full-tone sleep frame.
  ChargingBookPreparation* preparation = nullptr;  // Borrowed only while goToSleep waits.
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  bool renderCustomSleepBmp(const std::string& path) const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderDashboardSleepScreen() const;
  struct BitmapPlacement {
    int x, y, width, height;
    float cropX, cropY;
  };
  BitmapPlacement prepareSleepBitmap(Bitmap& bitmap) const;
  bool drawSleepBitmap(Bitmap& bitmap, const BitmapPlacement& placement) const;
  bool renderBitmapSleepScreen(Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen(Bitmap* cover = nullptr) const;
  mutable bool monochromeFrame = false;
  bool canSnapshotOverlayBackground = false;
  bool overlayBackgroundAvailable = false;
  bool quickResumeBackgroundAvailable = true;
  bool sleepPopupShown = false;
  std::string currentBookPath;
  bool fromTimeout = false;
  GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait;
  bool preserveSleepFrame;
};
