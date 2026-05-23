#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "activities/Activity.h"

struct BleTransferRuntime;

class BleTransferActivity final : public Activity {
 public:
  enum class State {
    STARTING,
    ADVERTISING,
    CONNECTED,
    RECEIVING_STATS,
    VERIFYING_STATS,
    SAVED_STATS,
    SENDING_STATS,
    SENT_STATS,
    SYNC_SCANNING,
    SYNC_CONNECTING,
    SYNCING_STATS,
    SYNCED_STATS,
    ERROR
  };

  explicit BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BleTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override {
    return state_ == State::VERIFYING_STATS || state_ == State::SENDING_STATS || state_ == State::SYNC_SCANNING ||
           state_ == State::SYNC_CONNECTING || state_ == State::SYNCING_STATS;
  }

  void enqueueBleConnected();
  void enqueueBleDisconnected();
  void enqueueControlWrite(const std::string& value);
  void enqueueDataWrite(const std::string& value);

 private:
  friend struct BleTransferRuntime;
  enum class BleEventType { CONNECTED, DISCONNECTED, CONTROL, DATA };
  struct BleEvent {
    BleEventType type;
    std::string value;
  };

  State state_ = State::STARTING;
  std::unique_ptr<BleTransferRuntime> ble_;
  FsFile uploadFile_;
  FsFile downloadFile_;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::deque<BleEvent> bleEvents_;
  size_t queuedBleEventBytes_ = 0;
  bool bleEventOverflow_ = false;

  std::string fileName_;
  std::string partPath_;
  std::string finalPath_;
  std::string expectedSha256_;
  std::string savedPath_;
  std::string errorMessage_;
  std::string deviceId_;

  size_t expectedSize_ = 0;
  size_t receivedBytes_ = 0;
  size_t sentBytes_ = 0;
  size_t lastProgressStatusBytes_ = 0;
  size_t lastDisplayProgressBytes_ = 0;
  size_t uploadAckBytes_ = 0;
  size_t downloadChunkSize_ = 0;
  uint32_t expectedSequence_ = 0;
  uint32_t downloadSequence_ = 0;
  uint32_t pendingDownloadAck_ = 0;
  bool helloAccepted_ = false;
  bool transferOpen_ = false;
  bool downloadOpen_ = false;
  bool downloadAwaitingAck_ = false;
  bool pendingCommit_ = false;
  bool statusDirty_ = true;
  bool removePartOnExit_ = false;
  bool shaActive_ = false;
  mbedtls_sha256_context shaContext_;

  void enqueueBleEvent(BleEvent event);
  void processBleEvents();
  void onBleConnected();
  void onBleDisconnected();
  void onControlWrite(const std::string& value);
  void onDataWrite(const std::string& value);
  void processCommit();
  void startStatsDownload(size_t offset, size_t chunkSize);
  void pumpDownload();
  void startPeerStatsSync();
  void resetTransfer(bool removePart);
  void setState(State state);
  void setError(const std::string& error);
  void publishStatus();
  std::string buildStatusJson() const;
  void renderReady(const std::string& primary, const std::string& secondary) const;
};
