#pragma once

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

class UsbDriveActivity final : public Activity {
 public:
  UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDrive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == State::Connected || state == State::Accessed || forcedDisconnectRequested ||
           (!startFailed && state == State::IoError);
  }
  bool requiresExclusiveStorageLoop() const override { return true; }

 private:
  enum class State { Unsupported, WaitingForHost, Connected, Accessed, Ejected, Disconnected, IoError };

  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 30UL * 1000UL;
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1000UL;

  void restartToHome();
  void renderMessage(const char* message, const char* detail = nullptr) const;

  State state = State::Unsupported;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;
  ScreenTransitionRefresh screenTransitionRefresh;
};
