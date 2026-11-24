#pragma once

#include <atomic>

class GfxRenderer;
class SleepActivity;

class ChargingBookPreparation {
 public:
  ChargingBookPreparation();
  static bool hasExternalPower();
  // Runs once on the reader-sized render stack. A cable-only boot passes no sleep screen
  // because the panel already holds its final image.
  void render(GfxRenderer& renderer, SleepActivity* sleep);
  // Main-task wake tracking lives through the final screen and storage shutdown.
  bool pollWake();
  bool finishWake();
  void wait();

 private:
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool shortWake;
  bool wakeArmed;
  bool wakeRequested = false;
  void scan(GfxRenderer& renderer, SleepActivity* sleep);
};
