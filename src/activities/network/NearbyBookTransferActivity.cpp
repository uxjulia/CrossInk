#include "NearbyBookTransferActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <esp_system.h>
#endif

namespace nearby = freeink::nearby;
namespace fui = freeink::ui;

namespace {
constexpr const char* LOG_TAG = "NBOOK";
constexpr uint8_t BROADCAST_MAC[nearby::MAC_BYTES] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint8_t RESULT_OK = 0;
constexpr uint8_t RESULT_FAILED = 1;
constexpr uint8_t REJECT_USER = 1;
constexpr uint8_t REJECT_STORAGE = 2;
TouchActionButtons::Layout touchActionLayout(const Rect& screen, const uint8_t count) {
  constexpr int sideMargin = 24;
  constexpr int bottomMargin = 12;
  const int totalHeight =
      TouchActionButtons::kDefaultHeight * count + TouchActionButtons::kDefaultGap * (count > 0 ? count - 1 : 0);
  return TouchActionButtons::vertical(Rect{screen.x + sideMargin, screen.y + screen.height - bottomMargin - totalHeight,
                                           std::max(1, screen.width - sideMargin * 2), totalHeight},
                                      count);
}

bool sameMac(const std::array<uint8_t, nearby::MAC_BYTES>& lhs, const uint8_t* rhs) {
  return rhs && memcmp(lhs.data(), rhs, nearby::MAC_BYTES) == 0;
}
}  // namespace

NearbyBookTransferActivity::NearbyBookTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const Mode mode, std::string sourcePath,
                                                       const bool returnToReader)
    : Activity("NearbyBookTransfer", renderer, mappedInput),
      mode_(mode),
      sourcePath_(std::move(sourcePath)),
      returnToReader_(returnToReader),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()) {}

NearbyBookTransferActivity::~NearbyBookTransferActivity() { stopRadio(); }

bool NearbyBookTransferActivity::supportedFile(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) ||
         FsHelpers::hasPngExtension(path) || FsHelpers::hasBmpExtension(path);
}

bool NearbyBookTransferActivity::safeFileName(const std::string& name) {
  if (name.empty() || name.size() > 180 || name.front() == '.' || name.back() == ' ' || name.back() == '.') {
    return false;
  }
  return name.find_first_of("/\\:*?\"<>|") == std::string::npos && supportedFile(name);
}

std::string NearbyBookTransferActivity::fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string NearbyBookTransferActivity::joinPath(const std::string& folder, const std::string& name) {
  if (folder.empty() || folder == "/") return "/" + name;
  return folder.back() == '/' ? folder + name : folder + "/" + name;
}

std::string NearbyBookTransferActivity::keepBothPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
  const std::string base = hasExtension ? path.substr(0, dot) : path;
  const std::string extension = hasExtension ? path.substr(dot) : "";
  for (uint16_t suffix = 2; suffix < 1000; ++suffix) {
    char number[12];
    snprintf(number, sizeof(number), " (%u)", static_cast<unsigned>(suffix));
    const std::string candidate = base + number + extension;
    if (!Storage.exists(candidate.c_str())) return candidate;
  }
  return {};
}

void NearbyBookTransferActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  destinationFolder_ = SETTINGS.nearbyReceiveFolder[0] ? SETTINGS.nearbyReceiveFolder : "/";
  uiReady_ = false;
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &NearbyBookTransferActivity::onRowEvent, this);
  app_.setScreen(&NearbyBookTransferActivity::menuScreen, this);

  if (mode_ == Mode::Receive) {
    setState(State::ChooseReceiveAction);
    return;
  }

  if (!supportedFile(sourcePath_)) {
    setError(tr(STR_NEARBY_TRANSFER_UNSUPPORTED));
    return;
  }
  HalFile probe;
  if (!Storage.openFileForRead(LOG_TAG, sourcePath_, probe)) {
    setError(tr(STR_NEARBY_TRANSFER_SOURCE_FAILED));
    return;
  }
  offeredFileSize_ = probe.fileSize64();
  probe.close();
  offeredFileName_ = fileNameFromPath(sourcePath_);
  if (!safeFileName(offeredFileName_)) {
    setError(tr(STR_NEARBY_TRANSFER_SOURCE_FAILED));
    return;
  }
  startDiscovery();
}

void NearbyBookTransferActivity::onExit() {
  sourceFile_.close();
  receiveFile_.close();
  stopRadio();
  Activity::onExit();
}

bool NearbyBookTransferActivity::skipLoopDelay() {
  return state_ == State::Discovering || state_ == State::WaitingForApproval || state_ == State::Sending ||
         state_ == State::Receiving;
}

bool NearbyBookTransferActivity::startRadio() {
  if (transport_.started()) return true;
  radioUsed_ = true;
  if (!transport_.begin(ESPNOW_CHANNEL)) {
    setError(tr(STR_NEARBY_TRANSFER_RADIO_FAILED));
    return false;
  }
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  LOG_INF(LOG_TAG, "radio ready: free=%u maxAlloc=%u stack=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
  return true;
}

void NearbyBookTransferActivity::stopRadio() { transport_.end(); }

void NearbyBookTransferActivity::startListening() {
  if (!startRadio()) return;
  session_.reset();
  peerMac_ = {};
  offeredFileName_.clear();
  senderName_.clear();
  finalPath_.clear();
  tempPath_.clear();
  retryCount_ = 0;
  setState(State::Listening);
}

void NearbyBookTransferActivity::startDiscovery() {
  if (!startRadio()) return;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  sessionId_ = esp_random();
#else
  sessionId_ = static_cast<uint32_t>(millis()) ^ 0x43494654u;
#endif
  if (sessionId_ == 0) sessionId_ = 1;
  peerCount_ = 0;
  selectedIndex_ = 0;
  lastActionMs_ = 0;
  setState(State::Discovering);
  sendDiscovery();
}

bool NearbyBookTransferActivity::sendPacket(const nearby::PacketType type, const uint8_t* destination,
                                            const uint32_t sequence, const void* payload,
                                            const uint16_t payloadLength) {
  size_t length = 0;
  if (!nearby::encodePacket(packetBuffer_.data(), packetBuffer_.size(), type, sessionId_, sequence, payload,
                            payloadLength, length)) {
    return false;
  }
  return transport_.send(destination, packetBuffer_.data(), length);
}

bool NearbyBookTransferActivity::sendDiscovery() {
  lastActionMs_ = millis();
  return sendPacket(nearby::PacketType::Discover, BROADCAST_MAC);
}

bool NearbyBookTransferActivity::sendAdvertisement(const uint8_t* destination) {
  const char* name = SETTINGS.getEffectiveDeviceName();
  const uint16_t length = static_cast<uint16_t>(std::min(strlen(name), CrossPointSettings::MAX_DEVICE_NAME_LENGTH));
  return sendPacket(nearby::PacketType::Advertise, destination, 0, name, length);
}

void NearbyBookTransferActivity::selectPeer() {
  if (peerCount_ == 0 || selectedIndex_ < 0 || selectedIndex_ >= peerCount_) return;
  peerMac_ = peers_[selectedIndex_].mac;
  retryCount_ = 0;
  setState(State::WaitingForApproval);
  sendOffer();
}

bool NearbyBookTransferActivity::sendOffer() {
  const char* deviceName = SETTINGS.getEffectiveDeviceName();
  const size_t senderLength = std::min(strlen(deviceName), CrossPointSettings::MAX_DEVICE_NAME_LENGTH);
  if (offeredFileName_.size() > 180 || senderLength == 0) return false;
  std::array<uint8_t, 212> payload{};
  nearby::writeU64(payload.data(), offeredFileSize_);
  nearby::writeU16(payload.data() + 8, nearby::V2_CHUNK_BYTES);
  payload[10] = static_cast<uint8_t>(senderLength);
  payload[11] = static_cast<uint8_t>(offeredFileName_.size());
  memcpy(payload.data() + 12, deviceName, senderLength);
  memcpy(payload.data() + 12 + senderLength, offeredFileName_.data(), offeredFileName_.size());
  lastActionMs_ = millis();
  return sendPacket(nearby::PacketType::Offer, peerMac_.data(), 0, payload.data(),
                    static_cast<uint16_t>(12 + senderLength + offeredFileName_.size()));
}

void NearbyBookTransferActivity::processPackets() {
  while (transport_.poll(eventBuffer_)) {
    nearby::PacketView packet;
    if (nearby::decodePacket(eventBuffer_.data.data(), eventBuffer_.length, packet)) handlePacket(eventBuffer_, packet);
  }
}

void NearbyBookTransferActivity::handlePacket(const nearby::EspNowTransport::Event& event,
                                              const nearby::PacketView& packet) {
  if (packet.type == nearby::PacketType::Discover && state_ == State::Listening) {
    sessionId_ = packet.sessionId;
    sendAdvertisement(event.sourceMac.data());
    return;
  }

  if (mode_ == Mode::Send && packet.sessionId == sessionId_) {
    if (packet.type == nearby::PacketType::Advertise && (state_ == State::Discovering || state_ == State::DeviceList) &&
        packet.payloadLength >= 2 && packet.payloadLength <= CrossPointSettings::MAX_DEVICE_NAME_LENGTH) {
      for (uint8_t i = 0; i < peerCount_; ++i) {
        if (sameMac(peers_[i].mac, event.sourceMac.data())) return;
      }
      if (peerCount_ >= MAX_PEERS) return;
      Peer& peer = peers_[peerCount_++];
      peer.mac = event.sourceMac;
      memcpy(peer.name.data(), packet.payload, packet.payloadLength);
      peer.name[packet.payloadLength] = '\0';
      if (state_ == State::Discovering)
        setState(State::DeviceList);
      else
        requestUpdate();
      return;
    }
    if (!sameMac(peerMac_, event.sourceMac.data())) return;
    if (packet.type == nearby::PacketType::Accept && state_ == State::WaitingForApproval && packet.payloadLength == 2) {
      negotiatedChunkBytes_ =
          std::clamp<uint16_t>(nearby::readU16(packet.payload), nearby::COMPAT_CHUNK_BYTES, nearby::V2_CHUNK_BYTES);
      if (!Storage.openFileForRead(LOG_TAG, sourcePath_, sourceFile_)) {
        setError(tr(STR_NEARBY_TRANSFER_SOURCE_FAILED));
        return;
      }
      session_.begin(nearby::ReliableTransferSession::Role::Sender, sessionId_, offeredFileSize_,
                     negotiatedChunkBytes_);
      retryCount_ = 0;
      setState(State::Sending);
      if (offeredFileSize_ == 0) {
        sourceFile_.close();
        sendComplete();
      } else {
        sendNextChunk();
      }
      return;
    }
    if (packet.type == nearby::PacketType::Reject && state_ == State::WaitingForApproval) {
      setError(tr(STR_NEARBY_TRANSFER_REJECTED));
      return;
    }
    if (packet.type == nearby::PacketType::Ack && state_ == State::Sending && packet.payloadLength == 4) {
      const uint32_t nextSequence = nearby::readU32(packet.payload);
      if (!session_.acceptAcknowledgement(nextSequence)) return;
      session_.advanceSentBytes(pendingChunkLength_);
      pendingChunkLength_ = 0;
      retryCount_ = 0;
      maybeRefreshProgress();
      if (session_.transferredBytes() == session_.totalBytes()) {
        sourceFile_.close();
        sendComplete();
      } else {
        sendNextChunk();
      }
      return;
    }
    if (packet.type == nearby::PacketType::Result && state_ == State::Sending && packet.payloadLength == 1) {
      if (packet.payload[0] == RESULT_OK) {
        setState(State::Success);
      } else {
        setError(tr(STR_NEARBY_TRANSFER_VERIFY_FAILED));
      }
      return;
    }
    if (packet.type == nearby::PacketType::Cancel) setError(tr(STR_NEARBY_TRANSFER_CANCELLED));
    return;
  }

  if (mode_ != Mode::Receive) return;
  if (packet.type == nearby::PacketType::Offer && state_ == State::Listening && packet.payloadLength >= 14) {
    const uint8_t senderLength = packet.payload[10];
    const uint8_t nameLength = packet.payload[11];
    if (senderLength == 0 || senderLength > CrossPointSettings::MAX_DEVICE_NAME_LENGTH || nameLength == 0 ||
        nameLength > 180 || packet.payloadLength != 12 + senderLength + nameLength) {
      return;
    }
    std::string name(reinterpret_cast<const char*>(packet.payload + 12 + senderLength), nameLength);
    if (!safeFileName(name)) return;
    sessionId_ = packet.sessionId;
    peerMac_ = event.sourceMac;
    offeredFileSize_ = nearby::readU64(packet.payload);
    negotiatedChunkBytes_ =
        std::clamp<uint16_t>(nearby::readU16(packet.payload + 8), nearby::COMPAT_CHUNK_BYTES, nearby::V2_CHUNK_BYTES);
    offeredFileName_ = std::move(name);
    senderName_.assign(reinterpret_cast<const char*>(packet.payload + 12), senderLength);
    finalPath_ = joinPath(destinationFolder_, offeredFileName_);
    setState(State::OfferPrompt);
    return;
  }
  if (packet.sessionId != sessionId_ || !sameMac(peerMac_, event.sourceMac.data())) return;
  if (packet.type == nearby::PacketType::Offer && state_ == State::Receiving) {
    if (acceptPending_) return;
    uint8_t payload[2];
    nearby::writeU16(payload, negotiatedChunkBytes_);
    sendPacket(nearby::PacketType::Accept, peerMac_.data(), 0, payload, sizeof(payload));
    return;
  }
  if (packet.type == nearby::PacketType::Data && state_ == State::Receiving) {
    if (packet.sequence < session_.nextSequence()) {
      sendAck();
      return;
    }
    if (packet.sequence != session_.nextSequence() || packet.payloadLength == 0 ||
        packet.payloadLength > negotiatedChunkBytes_) {
      sendAck();
      return;
    }
    const size_t written = receiveFile_.write(packet.payload, packet.payloadLength);
    if (written != packet.payloadLength ||
        !session_.acceptReceivedChunk(packet.sequence, static_cast<size_t>(packet.payloadLength))) {
      const uint8_t failed = RESULT_FAILED;
      sendPacket(nearby::PacketType::Result, peerMac_.data(), 0, &failed, 1);
      setError(tr(STR_NEARBY_TRANSFER_WRITE_FAILED));
      return;
    }
    session_.includeBytes(packet.payload, packet.payloadLength);
    sendAck();
    maybeRefreshProgress();
    return;
  }
  if (packet.type == nearby::PacketType::Complete && (state_ == State::Receiving || state_ == State::Success) &&
      packet.payloadLength == 12) {
    const uint64_t expectedBytes = nearby::readU64(packet.payload);
    const uint32_t expectedCrc = nearby::readU32(packet.payload + 8);
    const bool success = state_ == State::Success || finishReceivedFile(expectedBytes, expectedCrc);
    const uint8_t result = success ? RESULT_OK : RESULT_FAILED;
    sendPacket(nearby::PacketType::Result, peerMac_.data(), 0, &result, 1);
    if (success) setState(State::Success);
    return;
  }
  if (packet.type == nearby::PacketType::Cancel) {
    cancelTransfer();
    setError(tr(STR_NEARBY_TRANSFER_CANCELLED));
  }
}

bool NearbyBookTransferActivity::acceptOffer(const bool keepBoth) {
  receivingScreenDrawn_.store(false, std::memory_order_release);
  acceptPending_ = false;
  setState(State::Validating);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR(LOG_TAG, "Validation screen could not be rendered synchronously");
    requestUpdate(true);
  }

  if (keepBoth) {
    finalPath_ = keepBothPath(finalPath_);
    if (finalPath_.empty()) {
      setError(tr(STR_NEARBY_TRANSFER_NAME_FAILED));
      return false;
    }
  }
  uint64_t total = 0;
  uint64_t used = 0;
#ifndef SIMULATOR
  total = Storage.totalBytes();
  used = Storage.usedBytes();
#endif
  if (total > 0 && used <= total && offeredFileSize_ > total - used) {
    const uint8_t reason = REJECT_STORAGE;
    sendPacket(nearby::PacketType::Reject, peerMac_.data(), 0, &reason, 1);
    setError(tr(STR_NEARBY_TRANSFER_NO_SPACE));
    return false;
  }
  const std::string finalName = fileNameFromPath(finalPath_);
  tempPath_ = joinPath(destinationFolder_, "." + finalName + ".crossink-part");
  backupPath_ = joinPath(destinationFolder_, "." + finalName + ".crossink-backup");
  if (Storage.exists(tempPath_.c_str())) Storage.remove(tempPath_.c_str());
  if (!Storage.openFileForWrite(LOG_TAG, tempPath_, receiveFile_)) {
    setError(tr(STR_NEARBY_TRANSFER_WRITE_FAILED));
    return false;
  }
  session_.begin(nearby::ReliableTransferSession::Role::Receiver, sessionId_, offeredFileSize_, negotiatedChunkBytes_);
  retryCount_ = 0;
  lastActionMs_ = millis();
  acceptPending_ = true;
  setState(State::Receiving);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR(LOG_TAG, "Receiving screen could not be rendered synchronously");
    requestUpdate(true);
  }

  return true;
}

void NearbyBookTransferActivity::sendPendingAccept() {
  if (!acceptPending_ || !receivingScreenDrawn_.load(std::memory_order_acquire)) return;
  acceptPending_ = false;
  uint8_t payload[2];
  nearby::writeU16(payload, negotiatedChunkBytes_);
  if (!sendPacket(nearby::PacketType::Accept, peerMac_.data(), 0, payload, sizeof(payload))) {
    setError(tr(STR_NEARBY_TRANSFER_RADIO_FAILED));
    return;
  }
  lastActionMs_ = millis();
}

bool NearbyBookTransferActivity::sendNextChunk() {
  pendingChunkLength_ = 0;
  const int bytesRead = sourceFile_.read(chunkBuffer_.data(), negotiatedChunkBytes_);
  if (bytesRead <= 0) {
    setError(tr(STR_NEARBY_TRANSFER_SOURCE_FAILED));
    return false;
  }
  pendingChunkLength_ = static_cast<size_t>(bytesRead);
  session_.includeBytes(chunkBuffer_.data(), pendingChunkLength_);
  retryCount_ = 0;
  return resendPending();
}

bool NearbyBookTransferActivity::resendPending() {
  lastActionMs_ = millis();
  return sendPacket(nearby::PacketType::Data, peerMac_.data(), session_.nextSequence(), chunkBuffer_.data(),
                    static_cast<uint16_t>(pendingChunkLength_));
}

bool NearbyBookTransferActivity::sendAck() {
  uint8_t payload[4];
  nearby::writeU32(payload, session_.nextSequence());
  lastActionMs_ = millis();
  return sendPacket(nearby::PacketType::Ack, peerMac_.data(), 0, payload, sizeof(payload));
}

bool NearbyBookTransferActivity::sendComplete() {
  uint8_t payload[12];
  nearby::writeU64(payload, session_.totalBytes());
  nearby::writeU32(payload + 8, session_.crc32());
  lastActionMs_ = millis();
  retryCount_ = 0;
  return sendPacket(nearby::PacketType::Complete, peerMac_.data(), 0, payload, sizeof(payload));
}

bool NearbyBookTransferActivity::finishReceivedFile(const uint64_t expectedBytes, const uint32_t expectedCrc) {
  if (expectedBytes != session_.totalBytes() || expectedBytes != session_.transferredBytes() ||
      expectedCrc != session_.crc32()) {
    receiveFile_.close();
    Storage.remove(tempPath_.c_str());
    setError(tr(STR_NEARBY_TRANSFER_VERIFY_FAILED));
    return false;
  }
  receiveFile_.flush();
  const bool synced = receiveFile_.sync();
  const bool closed = receiveFile_.close();
  if (!synced || !closed) {
    Storage.remove(tempPath_.c_str());
    setError(tr(STR_NEARBY_TRANSFER_WRITE_FAILED));
    return false;
  }
  const bool replacing = Storage.exists(finalPath_.c_str());
  if (replacing) {
    if (Storage.exists(backupPath_.c_str())) Storage.remove(backupPath_.c_str());
    if (!Storage.rename(finalPath_.c_str(), backupPath_.c_str())) {
      Storage.remove(tempPath_.c_str());
      setError(tr(STR_NEARBY_TRANSFER_REPLACE_FAILED));
      return false;
    }
  }
  if (!Storage.rename(tempPath_.c_str(), finalPath_.c_str())) {
    if (replacing) Storage.rename(backupPath_.c_str(), finalPath_.c_str());
    Storage.remove(tempPath_.c_str());
    setError(tr(STR_NEARBY_TRANSFER_REPLACE_FAILED));
    return false;
  }
  if (replacing) Storage.remove(backupPath_.c_str());
  ImageFolderIndex::invalidateForPath(finalPath_.c_str());
  return true;
}

void NearbyBookTransferActivity::cancelTransfer() {
  acceptPending_ = false;
  receivingScreenDrawn_.store(false, std::memory_order_release);
  if (transport_.started() && peerMac_ != std::array<uint8_t, nearby::MAC_BYTES>{}) {
    sendPacket(nearby::PacketType::Cancel, peerMac_.data());
  }
  sourceFile_.close();
  receiveFile_.close();
  if (!tempPath_.empty() && Storage.exists(tempPath_.c_str())) Storage.remove(tempPath_.c_str());
}

void NearbyBookTransferActivity::rejectOffer() {
  const uint8_t reason = REJECT_USER;
  sendPacket(nearby::PacketType::Reject, peerMac_.data(), 0, &reason, 1);
  startListening();
}

void NearbyBookTransferActivity::setState(const State state) {
  state_ = state;
  selectedIndex_ = 0;
  uiReady_ = false;
  app_.clearTapFlash();
  lastUiMs_ = millis();
  requestUpdate();
}

void NearbyBookTransferActivity::setError(const char* message) {
  errorMessage_ = message ? message : tr(STR_NEARBY_TRANSFER_FAILED);
  LOG_ERR(LOG_TAG, "%s", errorMessage_.c_str());
  cancelTransfer();
  setState(State::Error);
}

void NearbyBookTransferActivity::exitAfterRadio() {
  if (state_ == State::Sending || state_ == State::Receiving || state_ == State::WaitingForApproval ||
      state_ == State::OfferPrompt || state_ == State::CollisionPrompt) {
    cancelTransfer();
  } else {
    sourceFile_.close();
    receiveFile_.close();
  }
  stopRadio();
  if (radioUsed_) {
    if (returnToReader_) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
  } else {
    onGoHome();
  }
}

void NearbyBookTransferActivity::openReceivedFile() {
  if (finalPath_.empty()) return;
  APP_STATE.openEpubPath = finalPath_;
  APP_STATE.saveToFile();
  stopRadio();
  silentRestartToReader();
}

void NearbyBookTransferActivity::chooseDestinationFolder() {
  auto picker = makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, destinationFolder_,
                                                       FileBrowserActivity::Mode::PickDirectory);
  if (!picker) {
    setError(tr(STR_NEARBY_TRANSFER_FAILED));
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    const auto* path = std::get_if<FilePathResult>(&result.data);
    if (result.isCancelled || !path) return;
    destinationFolder_ = path->path.empty() ? "/" : path->path;
    const std::string stored = destinationFolder_ == "/" ? "" : destinationFolder_;
    strncpy(SETTINGS.nearbyReceiveFolder, stored.c_str(), sizeof(SETTINGS.nearbyReceiveFolder) - 1);
    SETTINGS.nearbyReceiveFolder[sizeof(SETTINGS.nearbyReceiveFolder) - 1] = '\0';
    if (!SETTINGS.saveToFile()) LOG_ERR(LOG_TAG, "Failed to save receive folder");
    requestUpdate();
  });
}

void NearbyBookTransferActivity::updateTimers() {
  const uint32_t now = millis();
  if (state_ == State::Discovering && now - lastActionMs_ >= DISCOVERY_INTERVAL_MS) {
    sendDiscovery();
    return;
  }
  if (state_ == State::WaitingForApproval && now - lastActionMs_ >= RETRY_INTERVAL_MS * 2) {
    if (++retryCount_ > MAX_APPROVAL_RETRIES) {
      setError(tr(STR_NEARBY_TRANSFER_TIMEOUT));
    } else {
      sendOffer();
    }
    return;
  }
  if (state_ == State::Receiving && now - lastActionMs_ >= RECEIVE_TIMEOUT_MS) {
    setError(tr(STR_NEARBY_TRANSFER_TIMEOUT));
    return;
  }
  if (state_ != State::Sending || now - lastActionMs_ < RETRY_INTERVAL_MS) return;
  if (++retryCount_ > MAX_RETRIES) {
    setError(tr(STR_NEARBY_TRANSFER_TIMEOUT));
    return;
  }
  if (pendingChunkLength_ > 0) {
    resendPending();
  } else {
    sendComplete();
  }
}

void NearbyBookTransferActivity::maybeRefreshProgress() {
  if (millis() - lastUiMs_ < UI_REFRESH_MS) return;
  lastUiMs_ = millis();
  requestUpdate();
}

bool NearbyBookTransferActivity::isMenuState() const {
  return state_ == State::ChooseReceiveAction || state_ == State::DeviceList ||
         (state_ == State::CollisionPrompt && !mappedInput.hasTouch());
}

int NearbyBookTransferActivity::menuItemCount() const {
  if (state_ == State::ChooseReceiveAction) return 2;
  if (state_ == State::DeviceList) return peerCount_;
  if (state_ == State::CollisionPrompt) return 3;
  return 0;
}

void NearbyBookTransferActivity::activateSelected() {
  switch (state_) {
    case State::ChooseReceiveAction:
      if (selectedIndex_ == 0)
        startListening();
      else
        chooseDestinationFolder();
      break;
    case State::DeviceList:
      selectPeer();
      break;
    case State::OfferPrompt:
      if (Storage.exists(finalPath_.c_str()))
        setState(State::CollisionPrompt);
      else
        acceptOffer(false);
      break;
    case State::CollisionPrompt:
      if (selectedIndex_ == 0) {
        acceptOffer(false);
      } else if (selectedIndex_ == 1) {
        acceptOffer(true);
      } else {
        rejectOffer();
      }
      break;
    case State::Success:
      if (mode_ == Mode::Receive)
        openReceivedFile();
      else
        exitAfterRadio();
      break;
    case State::Error:
      exitAfterRadio();
      break;
    default:
      break;
  }
}

void NearbyBookTransferActivity::menuScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<NearbyBookTransferActivity*>(user)->buildMenuScreen(screen);
}

void NearbyBookTransferActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<NearbyBookTransferActivity*>(user);
  if (!self->isMenuState() || event.value < 0 || event.value >= self->menuItemCount()) return;
  self->selectedIndex_ = event.value;
  self->app_.clearTapFlash();
  self->activateSelected();
}

void NearbyBookTransferActivity::buildMenuScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.verticalSpacing),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  std::array<fui::ListItem, MAX_PEERS> items{};
  const int count = menuItemCount();
  for (int i = 0; i < count; ++i) {
    if (state_ == State::ChooseReceiveAction) {
      items[i].label = i == 0 ? tr(STR_START_RECEIVING) : tr(STR_CHANGE_FOLDER);
      if (i == 1) items[i].value = destinationFolder_.c_str();
    } else if (state_ == State::DeviceList) {
      items[i].label = peers_[i].name.data();
    } else if (state_ == State::CollisionPrompt) {
      items[i].label = i == 0 ? tr(STR_REPLACE) : (i == 1 ? tr(STR_KEEP_BOTH) : tr(STR_CANCEL));
    }
    items[i].actionValue = static_cast<int16_t>(i);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(count);
  props.selectedIndex = static_cast<int16_t>(selectedIndex_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  configureUiList(props, screen.theme(), screen.body());
  screen.list(props);
}

void NearbyBookTransferActivity::updateNavigation() {
  const int count = menuItemCount();
  if (count <= 1) return;
  buttonNavigator_.onNextRelease([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
}

void NearbyBookTransferActivity::loop() {
  sendPendingAccept();
  processPackets();
  updateTimers();

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state_ == State::OfferPrompt || state_ == State::CollisionPrompt) {
      const uint8_t reason = REJECT_USER;
      sendPacket(nearby::PacketType::Reject, peerMac_.data(), 0, &reason, 1);
      startListening();
    } else {
      exitAfterRadio();
    }
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.hasTouch() &&
      (state_ == State::OfferPrompt || state_ == State::CollisionPrompt || state_ == State::Success) &&
      mappedInput.wasScreenTouchDown(tx, ty)) {
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    if (state_ == State::CollisionPrompt) {
      const auto actions = touchActionLayout(safeArea, 3);
      const int index = TouchActionButtons::indexAt(actions, tx, ty);
      if (index >= 0) {
        selectedIndex_ = index;
        activateSelected();
      }
      return;
    }
    const uint8_t actionCount = state_ == State::Success && mode_ != Mode::Receive ? 1 : 2;
    const auto actions = touchActionLayout(safeArea, actionCount);
    const int action = TouchActionButtons::indexAt(actions, tx, ty);
    if (action >= 0) {
      if (actionCount == 2 && action == 1) {
        if (state_ == State::Success) {
          exitAfterRadio();
        } else {
          rejectOffer();
        }
      } else {
        activateSelected();
      }
      return;
    }
  }

  if (isMenuState() && uiReady_) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app_.route(snap);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  updateNavigation();
}

void NearbyBookTransferActivity::render(RenderLock&&) {
  renderer.clearScreen();
  bool drewReceivingScreen = false;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const Rect header{0, metrics.topPadding, width, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_NEARBY_BOOK_TRANSFER), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_NEARBY_BOOK_TRANSFER));
  }

  const Rect textArea{metrics.contentSidePadding, 0, width - metrics.contentSidePadding * 2, height};
  auto centered = [this, height, textArea](const char* text, const int offset = 0, const int font = UI_10_FONT_ID) {
    UITheme::drawCenteredWrappedTextAtCenter(renderer, textArea, font, height / 2 + offset, text, 2, true,
                                             offset == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  };
  auto centeredWrapped = [this, height, textArea](const char* text, const int offset, const int maxLines,
                                                  const int font = UI_10_FONT_ID) {
    UITheme::drawCenteredWrappedTextAtCenter(renderer, textArea, font, height / 2 + offset, text, maxLines);
  };
  uiReady_ = false;
  if (isMenuState()) {
    app_.render();
    uiReady_ = true;
  } else if (state_ == State::Listening) {
    centered(tr(STR_NEARBY_TRANSFER_LISTENING));
    centeredWrapped(destinationFolder_.c_str(), renderer.getLineHeight(UI_10_FONT_ID) + 10, 2, SMALL_FONT_ID);
  } else if (state_ == State::Discovering) {
    centered(tr(STR_NEARBY_TRANSFER_DISCOVERING));
  } else if (state_ == State::WaitingForApproval) {
    centeredWrapped(tr(STR_NEARBY_TRANSFER_WAITING_APPROVAL), -10, 2);
    centeredWrapped(peers_[selectedIndex_].name.data(), renderer.getLineHeight(UI_10_FONT_ID) + 18, 2, SMALL_FONT_ID);
  } else if (state_ == State::Validating) {
    centered(tr(STR_NEARBY_TRANSFER_VALIDATING));
  } else if (state_ == State::OfferPrompt) {
    centeredWrapped(tr(STR_NEARBY_TRANSFER_INCOMING), -65, 2);
    centeredWrapped(senderName_.c_str(), -25, 2, SMALL_FONT_ID);
    centeredWrapped(offeredFileName_.c_str(), 25, 2, SMALL_FONT_ID);
    char sizeText[48];
    snprintf(sizeText, sizeof(sizeText), tr(STR_NEARBY_TRANSFER_SIZE), static_cast<unsigned long>(offeredFileSize_));
    centered(sizeText, 70, SMALL_FONT_ID);
  } else if (state_ == State::CollisionPrompt) {
    centeredWrapped(offeredFileName_.c_str(), -150, 2, SMALL_FONT_ID);
  } else if (state_ == State::Sending || state_ == State::Receiving) {
    centeredWrapped(state_ == State::Sending ? tr(STR_NEARBY_TRANSFER_SENDING) : tr(STR_NEARBY_TRANSFER_RECEIVING), -45,
                    2);
    drewReceivingScreen = state_ == State::Receiving;
    const uint64_t scale = std::max<uint64_t>(1, session_.totalBytes() / 10000 + 1);
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, height / 2 + 10, width - metrics.contentSidePadding * 2,
                             metrics.progressBarHeight},
                        static_cast<size_t>(session_.transferredBytes() / scale),
                        static_cast<size_t>(session_.totalBytes() / scale));
  } else if (state_ == State::Success) {
    centered(mode_ == Mode::Receive ? tr(STR_NEARBY_TRANSFER_RECEIVED) : tr(STR_NEARBY_TRANSFER_SENT));
    centeredWrapped(mode_ == Mode::Receive ? finalPath_.c_str() : offeredFileName_.c_str(),
                    renderer.getLineHeight(UI_10_FONT_ID) + 10, 3, SMALL_FONT_ID);
  } else if (state_ == State::Error) {
    centered(tr(STR_NEARBY_TRANSFER_FAILED));
    centeredWrapped(errorMessage_.c_str(), renderer.getLineHeight(UI_10_FONT_ID) + 10, 3, SMALL_FONT_ID);
  }

  std::array<char, 128> backLabelBuffer{};
  const char* back = tr(STR_CANCEL);
  const char* confirm = "";
  if (state_ == State::ChooseReceiveAction || state_ == State::DeviceList || state_ == State::CollisionPrompt)
    confirm = tr(STR_SELECT);
  else if (state_ == State::OfferPrompt)
    confirm = tr(STR_ACCEPT);
  else if (state_ == State::Success && mode_ == Mode::Receive) {
    std::snprintf(backLabelBuffer.data(), backLabelBuffer.size(), "%s",
                  mappedInput.resolveLabel(mappedInput.withBackArrow(tr(STR_BACK))));
    back = backLabelBuffer.data();
    confirm =
        FsHelpers::hasPngExtension(finalPath_) || FsHelpers::hasBmpExtension(finalPath_) ? tr(STR_OPEN) : tr(STR_READ);
  } else if (state_ == State::Success) {
    back = "";
    confirm = tr(STR_OK);
  } else if (state_ == State::Error)
    confirm = tr(STR_OK);
  const bool showNavigation = !mappedInput.hasTouch() && menuItemCount() > 1;
  const bool showTouchCollisionActions = mappedInput.hasTouch() && state_ == State::CollisionPrompt;
  const bool showTouchActions = mappedInput.hasTouch() && (state_ == State::OfferPrompt || state_ == State::Success);
  if (showTouchCollisionActions) {
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const auto actions = touchActionLayout(safeArea, 3);
    const char* actionLabels[] = {tr(STR_REPLACE), tr(STR_KEEP_BOTH), tr(STR_CANCEL)};
    TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
  } else if (showTouchActions) {
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const bool hasBack = state_ == State::OfferPrompt || mode_ == Mode::Receive;
    const uint8_t actionCount = hasBack ? 2 : 1;
    const auto actions = touchActionLayout(safeArea, actionCount);
    const char* actionLabels[] = {confirm, hasBack ? back : nullptr};
    TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
  } else {
    const auto labels = mappedInput.mapLabels(back, confirm, showNavigation ? tr(STR_DIR_UP) : "",
                                              showNavigation ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
  if (drewReceivingScreen) receivingScreenDrawn_.store(true, std::memory_order_release);
}
