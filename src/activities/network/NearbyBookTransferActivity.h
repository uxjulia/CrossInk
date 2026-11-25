#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <HalStorage.h>
#include <NearbyTransfer.h>

#include <array>
#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"

class NearbyBookTransferActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Send, Receive };

  NearbyBookTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode,
                             std::string sourcePath = {}, bool returnToReader = false);
  ~NearbyBookTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override;

 private:
  enum class State : uint8_t {
    ChooseReceiveAction,
    Listening,
    Discovering,
    DeviceList,
    WaitingForApproval,
    OfferPrompt,
    CollisionPrompt,
    Validating,
    Sending,
    Receiving,
    Success,
    Error,
  };

  struct Peer {
    std::array<uint8_t, freeink::nearby::MAC_BYTES> mac{};
    std::array<char, 21> name{};
  };

  static constexpr size_t MAX_PEERS = 4;
  static constexpr uint8_t ESPNOW_CHANNEL = 1;
  static constexpr uint32_t DISCOVERY_INTERVAL_MS = 900;
  static constexpr uint32_t RETRY_INTERVAL_MS = 450;
  static constexpr uint8_t MAX_RETRIES = 12;
  static constexpr uint8_t MAX_APPROVAL_RETRIES = 60;
  static constexpr uint32_t RECEIVE_TIMEOUT_MS = 15000;
  static constexpr uint32_t UI_REFRESH_MS = 1800;

  Mode mode_;
  State state_ = State::ChooseReceiveAction;
  ScreenTransitionRefresh screenTransitionRefresh_;
  std::string sourcePath_;
  bool returnToReader_ = false;
  bool radioUsed_ = false;

  freeink::nearby::EspNowTransport transport_;
  freeink::nearby::EspNowTransport::Event eventBuffer_;
  freeink::nearby::ReliableTransferSession session_;
  std::array<Peer, MAX_PEERS> peers_{};
  uint8_t peerCount_ = 0;
  int selectedIndex_ = 0;
  std::array<uint8_t, freeink::nearby::MAC_BYTES> peerMac_{};

  HalFile sourceFile_;
  HalFile receiveFile_;
  std::array<uint8_t, freeink::nearby::V2_CHUNK_BYTES> chunkBuffer_{};
  std::array<uint8_t, freeink::nearby::MAX_PACKET_BYTES> packetBuffer_{};
  size_t pendingChunkLength_ = 0;
  uint64_t offeredFileSize_ = 0;
  uint16_t negotiatedChunkBytes_ = freeink::nearby::COMPAT_CHUNK_BYTES;
  uint32_t sessionId_ = 0;
  uint32_t lastActionMs_ = 0;
  uint32_t lastUiMs_ = 0;
  uint8_t retryCount_ = 0;

  std::string offeredFileName_;
  std::string senderName_;
  std::string destinationFolder_;
  std::string finalPath_;
  std::string tempPath_;
  std::string errorMessage_;

  ButtonNavigator buttonNavigator_;

  using UiApp = freeink::ui::FreeInkApp<8, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget_;  // Must precede app_: app_ holds a reference to it.
  UiApp app_;
  std::atomic<bool> uiReady_{false};
  std::atomic<bool> receivingScreenDrawn_{false};
  bool acceptPending_ = false;
  bool replaceExisting_ = false;

  bool startRadio();
  void stopRadio();
  void startListening();
  void startDiscovery();
  void selectPeer();
  void processPackets();
  void handlePacket(const freeink::nearby::EspNowTransport::Event& event, const freeink::nearby::PacketView& packet);
  bool sendPacket(freeink::nearby::PacketType type, const uint8_t* destination, uint32_t sequence = 0,
                  const void* payload = nullptr, uint16_t payloadLength = 0);
  bool sendDiscovery();
  bool sendAdvertisement(const uint8_t* destination);
  bool sendOffer();
  bool acceptOffer(bool keepBoth);
  void sendPendingAccept();
  bool sendNextChunk();
  bool resendPending();
  bool sendAck();
  bool sendComplete();
  bool finishReceivedFile(uint64_t expectedBytes, uint32_t expectedCrc);
  void cancelTransfer();
  void rejectOffer();
  void setState(State state);
  void setError(const char* message);
  void exitAfterRadio();
  void openReceivedFile();
  void chooseDestinationFolder();
  void updateTimers();
  void updateNavigation();
  void maybeRefreshProgress();
  bool isMenuState() const;
  int menuItemCount() const;
  void activateSelected();
  static void menuScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildMenuScreen(UiApp::ScreenType& screen);

  static bool supportedFile(const std::string& path);
  static bool safeFileName(const std::string& name);
  static std::string fileNameFromPath(const std::string& path);
  static std::string joinPath(const std::string& folder, const std::string& name);
  static std::string keepBothPath(const std::string& path);
};
