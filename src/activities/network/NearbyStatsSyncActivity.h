#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

class NearbyStatsSyncActivity final : public Activity {
 public:
  enum class State { STARTING, READY, DISCOVERING, SYNCING, SYNCED, ERROR };

  explicit NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~NearbyStatsSyncActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state_ == State::DISCOVERING || state_ == State::SYNCING; }

  void enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, int length);

 private:
  enum class PacketType : uint8_t { HELLO = 1, STATS = 2, ACK = 3 };

  struct SyncEvent {
    PacketType type = PacketType::HELLO;
    std::array<uint8_t, 6> sourceMac = {};
    std::array<uint8_t, 6> deviceMac = {};
    std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> stats = {};
    uint8_t statsSize = 0;
  };
  static constexpr size_t MAX_SYNC_EVENTS = 8;

  State state_ = State::STARTING;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::array<SyncEvent, MAX_SYNC_EVENTS> events_ = {};
  uint8_t eventHead_ = 0;
  uint8_t eventCount_ = 0;
  bool eventOverflow_ = false;
  bool espNowStarted_ = false;
  bool localStatsReady_ = false;
  bool peerSeen_ = false;
  bool peerStatsSaved_ = false;
  bool localStatsSent_ = false;
  bool localStatsAcked_ = false;

  std::array<uint8_t, 6> localDeviceMac_ = {};
  std::array<uint8_t, 6> peerSourceMac_ = {};
  std::array<uint8_t, 6> peerDeviceMac_ = {};
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> localStats_ = {};
  uint8_t localStatsSize_ = 0;

  uint32_t syncStartedMs_ = 0;
  uint32_t lastHelloMs_ = 0;
  uint32_t lastStatsSendMs_ = 0;
  std::string peerId_;
  std::string errorMessage_;

  bool beginEspNow();
  void endEspNow();
  bool prepareLocalStats();
  void startSync();
  void processEvents();
  void handleEvent(const SyncEvent& event);
  bool sendPacket(PacketType type, const uint8_t* peerMac);
  bool sendHello();
  bool sendLocalStats();
  bool sendAck(const uint8_t* peerMac);
  bool addPeer(const uint8_t* peerMac);
  void updateSyncProgress();
  void setState(State state);
  void setError(const std::string& error);
  void renderReady(const std::string& primary, const std::string& secondary) const;
};
