#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground)
      : Activity("Sleep", renderer, mappedInput), canSnapshotOverlayBackground(canSnapshotOverlayBackground) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  bool canSnapshotOverlayBackground = false;
  bool overlayPageBufferStored = false;
  bool overlayPageBufferTrusted = false;
};
