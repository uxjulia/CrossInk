#pragma once

#include "activities/Activity.h"

class BackupStatsActivity final : public Activity {
 public:
  explicit BackupStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BackupStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, SUCCESS, FAILED };

  State state = WARNING;
  char backupFileName[64] = {};

  void goBack() { finish(); }
  void runBackup();
};
