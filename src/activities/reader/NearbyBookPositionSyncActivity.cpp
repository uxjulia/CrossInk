#include "NearbyBookPositionSyncActivity.h"

#ifdef SIMULATOR

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

NearbyBookPositionSyncActivity::NearbyBookPositionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                               std::shared_ptr<Epub> epub, const std::string& epubPath,
                                                               int currentSpineIndex, int currentPage,
                                                               int totalPagesInSpine, KOReaderPosition localKoPos,
                                                               std::string localChapterName,
                                                               std::optional<uint16_t> currentParagraphIndex)
    : Activity("NearbyBookPositionSync", renderer, mappedInput),
      epub_(std::move(epub)),
      epubPath_(epubPath),
      localChapterName_(std::move(localChapterName)),
      currentSpineIndex_(currentSpineIndex),
      currentPage_(currentPage),
      totalPagesInSpine_(totalPagesInSpine),
      currentParagraphIndex_(currentParagraphIndex),
      localKoPosition_(std::move(localKoPos)) {}

NearbyBookPositionSyncActivity::~NearbyBookPositionSyncActivity() = default;

void NearbyBookPositionSyncActivity::onEnter() {
  Activity::onEnter();
  setState(State::ERROR);
  errorMessage_ = tr(STR_NEARBY_POSITION_SIMULATOR_UNAVAILABLE);
}

void NearbyBookPositionSyncActivity::onExit() { Activity::onExit(); }

void NearbyBookPositionSyncActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) returnToReader();
}

void NearbyBookPositionSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_POSITION_SYNC));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, errorMessage_.c_str(), true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void NearbyBookPositionSyncActivity::enqueueEspNowPacket(const uint8_t*, const uint8_t*, int) {}
bool NearbyBookPositionSyncActivity::prepareLocalPosition() { return false; }
bool NearbyBookPositionSyncActivity::ensureEpubLoaded() { return false; }
bool NearbyBookPositionSyncActivity::beginEspNow() { return false; }
void NearbyBookPositionSyncActivity::endEspNow() {}
void NearbyBookPositionSyncActivity::startSync() {}
void NearbyBookPositionSyncActivity::processEvents() {}
void NearbyBookPositionSyncActivity::handleEvent(const SyncEvent&) {}
bool NearbyBookPositionSyncActivity::sendPacket(PacketType, const uint8_t*) { return false; }
bool NearbyBookPositionSyncActivity::sendHello() { return false; }
bool NearbyBookPositionSyncActivity::sendDeviceName(const uint8_t*) { return false; }
bool NearbyBookPositionSyncActivity::sendLocalPosition() { return false; }
bool NearbyBookPositionSyncActivity::sendAck(const uint8_t*) { return false; }
bool NearbyBookPositionSyncActivity::addPeer(const uint8_t*) { return false; }
bool NearbyBookPositionSyncActivity::applyPeerPosition() { return false; }
bool NearbyBookPositionSyncActivity::mapPeerPosition() { return false; }
void NearbyBookPositionSyncActivity::updateSyncProgress() {}
void NearbyBookPositionSyncActivity::returnToReader() { finish(); }
void NearbyBookPositionSyncActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}
void NearbyBookPositionSyncActivity::setError(const std::string& error) {
  errorMessage_ = error;
  setState(State::ERROR);
}
void NearbyBookPositionSyncActivity::renderReady(const std::string&, const std::string&) const {}
void NearbyBookPositionSyncActivity::renderComparison() const {}

#else

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* LOG_TAG = "NBPS";
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t PACKET_HEADER_BYTES = 14;
constexpr uint16_t MAX_PACKET_BYTES = 250;
constexpr uint32_t HELLO_INTERVAL_MS = 750;
constexpr uint32_t POSITION_RETRY_INTERVAL_MS = 750;
constexpr uint32_t SYNC_TIMEOUT_MS = 15000;
constexpr uint32_t PERCENTAGE_SCALE = 1000000;
constexpr uint8_t BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint8_t FLAG_PARAGRAPH = 1 << 0;
constexpr uint8_t FLAG_LI = 1 << 1;

NearbyBookPositionSyncActivity* activeActivity = nullptr;

std::string bytesToHex(const uint8_t* data, const size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.resize(length * 2);
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = hex[data[i] >> 4];
    out[i * 2 + 1] = hex[data[i] & 0x0F];
  }
  return out;
}

bool isZeroMac(const std::array<uint8_t, 6>& mac) { return mac == std::array<uint8_t, 6>{}; }

void writeU16(uint8_t*& out, const uint16_t value) {
  *out++ = value & 0xFF;
  *out++ = (value >> 8) & 0xFF;
}

void writeU32(uint8_t*& out, const uint32_t value) {
  *out++ = value & 0xFF;
  *out++ = (value >> 8) & 0xFF;
  *out++ = (value >> 16) & 0xFF;
  *out++ = (value >> 24) & 0xFF;
}

bool readU16(const uint8_t*& in, size_t& remaining, uint16_t& value) {
  if (remaining < 2) return false;
  value = static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
  in += 2;
  remaining -= 2;
  return true;
}

bool readU32(const uint8_t*& in, size_t& remaining, uint32_t& value) {
  if (remaining < 4) return false;
  value = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
          (static_cast<uint32_t>(in[3]) << 24);
  in += 4;
  remaining -= 4;
  return true;
}

uint16_t clampU16(const int value, const uint16_t fallback = 0) {
  if (value < 0) return fallback;
  if (value > UINT16_MAX) return UINT16_MAX;
  return static_cast<uint16_t>(value);
}

uint32_t percentageToQ(const float percentage) {
  const float clamped = std::max(0.0f, std::min(1.0f, percentage));
  return static_cast<uint32_t>(std::lround(clamped * static_cast<float>(PERCENTAGE_SCALE)));
}

float qToPercentage(const uint32_t percentageQ) {
  return static_cast<float>(std::min(percentageQ, PERCENTAGE_SCALE)) / static_cast<float>(PERCENTAGE_SCALE);
}

size_t boundedLength(const char* text, const size_t maxBytes) {
  if (!text) return 0;
  size_t len = 0;
  while (len < maxBytes && text[len] != '\0') len++;
  return len;
}

void copyBounded(char* dest, const size_t destSize, const char* src, const size_t srcLen) {
  if (!dest || destSize == 0) return;
  const size_t n = std::min(destSize - 1, srcLen);
  if (n > 0 && src) memcpy(dest, src, n);
  dest[n] = '\0';
}

bool writeDocumentHash(uint8_t*& out,
                       const std::array<char, NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES + 1>& hash) {
  if (boundedLength(hash.data(), NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES) !=
      NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES) {
    return false;
  }
  memcpy(out, hash.data(), NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES);
  out += NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES;
  return true;
}

bool serializePosition(const NearbyBookPositionSyncActivity::CompactPosition& pos, uint8_t* out,
                       const size_t outCapacity, size_t& outLen) {
  constexpr size_t fixedBytes = NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES + 4 + 2 + 2 + 2 + 1 + 2 + 2 + 1 + 1;
  uint8_t* cursor = out;
  if (!out || outCapacity < fixedBytes || !writeDocumentHash(cursor, pos.documentHash)) return false;
  writeU32(cursor, std::min(pos.percentageQ, PERCENTAGE_SCALE));
  writeU16(cursor, pos.spineIndex);
  writeU16(cursor, pos.pageNumber);
  writeU16(cursor, std::max<uint16_t>(1, pos.totalPages));

  uint8_t flags = 0;
  if (pos.hasParagraphIndex) flags |= FLAG_PARAGRAPH;
  if (pos.hasLiIndex) flags |= FLAG_LI;
  *cursor++ = flags;
  writeU16(cursor, pos.paragraphIndex);
  writeU16(cursor, pos.liIndex);

  const size_t anchorLen = boundedLength(pos.anchor.data(), NearbyBookPositionSyncActivity::MAX_ANCHOR_BYTES);
  const size_t xpathLen = boundedLength(pos.xpath.data(), NearbyBookPositionSyncActivity::MAX_XPATH_BYTES);
  if (fixedBytes + anchorLen + xpathLen > outCapacity) return false;

  *cursor++ = static_cast<uint8_t>(anchorLen);
  if (anchorLen > 0) {
    memcpy(cursor, pos.anchor.data(), anchorLen);
    cursor += anchorLen;
  }
  *cursor++ = static_cast<uint8_t>(xpathLen);
  if (xpathLen > 0) {
    memcpy(cursor, pos.xpath.data(), xpathLen);
    cursor += xpathLen;
  }

  outLen = static_cast<size_t>(cursor - out);
  return true;
}

bool parsePosition(const uint8_t* data, const size_t len, NearbyBookPositionSyncActivity::CompactPosition& out) {
  constexpr size_t minBytes = NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES + 4 + 2 + 2 + 2 + 1 + 2 + 2 + 1 + 1;
  if (!data || len < minBytes) return false;

  const uint8_t* cursor = data;
  size_t remaining = len;
  memcpy(out.documentHash.data(), cursor, NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES);
  out.documentHash[NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES] = '\0';
  cursor += NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES;
  remaining -= NearbyBookPositionSyncActivity::DOCUMENT_HASH_BYTES;

  if (!readU32(cursor, remaining, out.percentageQ) || !readU16(cursor, remaining, out.spineIndex) ||
      !readU16(cursor, remaining, out.pageNumber) || !readU16(cursor, remaining, out.totalPages)) {
    return false;
  }
  out.percentageQ = std::min(out.percentageQ, PERCENTAGE_SCALE);
  out.totalPages = std::max<uint16_t>(1, out.totalPages);

  if (remaining < 1) return false;
  const uint8_t flags = *cursor++;
  remaining--;
  out.hasParagraphIndex = (flags & FLAG_PARAGRAPH) != 0;
  out.hasLiIndex = (flags & FLAG_LI) != 0;
  if (!readU16(cursor, remaining, out.paragraphIndex) || !readU16(cursor, remaining, out.liIndex) || remaining < 1) {
    return false;
  }

  const uint8_t anchorLen = *cursor++;
  remaining--;
  if (anchorLen > NearbyBookPositionSyncActivity::MAX_ANCHOR_BYTES || remaining < anchorLen + 1) return false;
  copyBounded(out.anchor.data(), out.anchor.size(), reinterpret_cast<const char*>(cursor), anchorLen);
  cursor += anchorLen;
  remaining -= anchorLen;

  const uint8_t xpathLen = *cursor++;
  remaining--;
  if (xpathLen > NearbyBookPositionSyncActivity::MAX_XPATH_BYTES || remaining != xpathLen) return false;
  copyBounded(out.xpath.data(), out.xpath.size(), reinterpret_cast<const char*>(cursor), xpathLen);
  return true;
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (!activeActivity || !info || !info->src_addr) return;
  activeActivity->enqueueEspNowPacket(info->src_addr, data, length);
}

}  // namespace

NearbyBookPositionSyncActivity::NearbyBookPositionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                               std::shared_ptr<Epub> epub, const std::string& epubPath,
                                                               int currentSpineIndex, int currentPage,
                                                               int totalPagesInSpine, KOReaderPosition localKoPos,
                                                               std::string localChapterName,
                                                               std::optional<uint16_t> currentParagraphIndex)
    : Activity("NearbyBookPositionSync", renderer, mappedInput),
      eventMutex_(xSemaphoreCreateMutex()),
      epub_(std::move(epub)),
      epubPath_(epubPath),
      localChapterName_(std::move(localChapterName)),
      currentSpineIndex_(currentSpineIndex),
      currentPage_(currentPage),
      totalPagesInSpine_(std::max(1, totalPagesInSpine)),
      currentParagraphIndex_(currentParagraphIndex),
      localKoPosition_(std::move(localKoPos)) {}

NearbyBookPositionSyncActivity::~NearbyBookPositionSyncActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

bool NearbyBookPositionSyncActivity::ensureEpubLoaded() {
  if (epub_) return true;
  LOG_ERR(LOG_TAG, "No EPUB available for nearby position sync");
  return false;
}

bool NearbyBookPositionSyncActivity::prepareLocalPosition() {
  if (localPrepared_) return true;

  const std::string documentHash = (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME)
                                       ? KOReaderDocumentId::calculateFromFilename(epubPath_)
                                       : KOReaderDocumentId::calculate(epubPath_);
  if (documentHash.size() != DOCUMENT_HASH_BYTES) {
    setError(tr(STR_HASH_FAILED));
    return false;
  }

  copyBounded(localPosition_.documentHash.data(), localPosition_.documentHash.size(), documentHash.c_str(),
              documentHash.size());
  localPosition_.percentageQ = percentageToQ(localKoPosition_.percentage);
  localPosition_.spineIndex = clampU16(currentSpineIndex_);
  localPosition_.pageNumber = clampU16(currentPage_);
  localPosition_.totalPages = clampU16(totalPagesInSpine_, 1);
  if (currentParagraphIndex_.has_value() && *currentParagraphIndex_ != UINT16_MAX) {
    localPosition_.paragraphIndex = *currentParagraphIndex_;
    localPosition_.hasParagraphIndex = true;
  }
  if (localKoPosition_.xpath.size() <= MAX_XPATH_BYTES) {
    copyBounded(localPosition_.xpath.data(), localPosition_.xpath.size(), localKoPosition_.xpath.c_str(),
                localKoPosition_.xpath.size());
  } else {
    LOG_DBG(LOG_TAG, "Omitting long XPath from ESP-NOW payload (%u bytes)", (unsigned)localKoPosition_.xpath.size());
  }

  if (!ensureEpubLoaded()) {
    setError(tr(STR_SYNC_FAILED_MSG));
    return false;
  }

  localPrepared_ = true;
  return true;
}

void NearbyBookPositionSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  setState(State::STARTING);

  if (esp_efuse_mac_get_default(localDeviceMac_.data()) != ESP_OK) {
    setError(tr(STR_NEARBY_POSITION_DEVICE_ID_FAILED));
    return;
  }
  if (!prepareLocalPosition()) return;
  if (!beginEspNow()) {
    setError(tr(STR_NEARBY_POSITION_START_FAILED));
    return;
  }

  setState(State::READY);
}

void NearbyBookPositionSyncActivity::onExit() {
  Activity::onExit();
  endEspNow();
}

void NearbyBookPositionSyncActivity::loop() {
  processEvents();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    returnToReader();
    return;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startSync();
      return;
    }
  } else if (state_ == State::SHOWING_RESULT && !sourceMode_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (applyPeerPosition()) {
        sendAck(peerSourceMac_.data());
        returnToReader();
      }
      return;
    }
  }

  updateSyncProgress();
}

bool NearbyBookPositionSyncActivity::beginEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.setSleep(false);
  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) return false;
  espNowStarted_ = true;

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) return false;
  if (!addPeer(BROADCAST_MAC)) return false;
  activeActivity = this;
  return true;
}

void NearbyBookPositionSyncActivity::endEspNow() {
  if (activeActivity == this) activeActivity = nullptr;
  if (espNowStarted_) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    espNowStarted_ = false;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void NearbyBookPositionSyncActivity::startSync() {
  errorMessage_.clear();
  peerSeen_ = false;
  peerPositionReceived_ = false;
  sourceMode_ = true;
  localPositionSent_ = false;
  localPositionAcked_ = false;
  peerSourceMac_ = {};
  peerDeviceMac_ = {};
  peerId_.clear();
  peerName_.clear();
  syncStartedMs_ = millis();
  lastHelloMs_ = 0;
  lastPositionSendMs_ = 0;

  if (!prepareLocalPosition()) return;

  setState(State::DISCOVERING);
  sendHello();
}

void NearbyBookPositionSyncActivity::enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data,
                                                         const int length) {
  if (!eventMutex_ || !sourceMac || !data || length < PACKET_HEADER_BYTES) return;
  if (data[0] != 'C' || data[1] != 'I' || data[2] != 'B' || data[3] != 'P') return;

  SyncEvent event;
  std::copy(sourceMac, sourceMac + event.sourceMac.size(), event.sourceMac.begin());
  std::copy(data + 8, data + 14, event.deviceMac.begin());
  if (event.deviceMac == localDeviceMac_) return;

  auto queueEvent = [&]() {
    if (xSemaphoreTake(eventMutex_, 0) != pdTRUE) return;
    if (eventOverflow_ || eventCount_ >= MAX_SYNC_EVENTS) {
      eventOverflow_ = true;
      eventHead_ = 0;
      eventCount_ = 0;
    } else {
      const uint8_t eventTail = static_cast<uint8_t>((eventHead_ + eventCount_) % MAX_SYNC_EVENTS);
      events_[eventTail] = event;
      eventCount_++;
    }
    xSemaphoreGive(eventMutex_);
  };

  if (data[4] != PROTOCOL_VERSION) {
    event.type = PacketType::INVALID;
    queueEvent();
    return;
  }

  const PacketType packetType = static_cast<PacketType>(data[5]);
  const uint16_t payloadLength = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
  if (length != static_cast<int>(PACKET_HEADER_BYTES + payloadLength) || length > MAX_PACKET_BYTES) return;

  event.type = packetType;
  const uint8_t* payload = data + PACKET_HEADER_BYTES;
  if (packetType == PacketType::HELLO) {
    if (payloadLength != DOCUMENT_HASH_BYTES) return;
    copyBounded(event.position.documentHash.data(), event.position.documentHash.size(),
                reinterpret_cast<const char*>(payload), DOCUMENT_HASH_BYTES);
  } else if (packetType == PacketType::POSITION || packetType == PacketType::APPLY) {
    if (!parsePosition(payload, payloadLength, event.position)) {
      event.type = PacketType::INVALID;
    }
  } else if (packetType == PacketType::NAME) {
    if (payloadLength < CrossPointSettings::MIN_DEVICE_NAME_LENGTH || payloadLength > MAX_DEVICE_NAME_BYTES) return;
    copyBounded(event.deviceName.data(), event.deviceName.size(), reinterpret_cast<const char*>(payload),
                payloadLength);
  } else if (packetType == PacketType::ACK) {
    if (payloadLength != 0) return;
  } else {
    return;
  }

  queueEvent();
}

void NearbyBookPositionSyncActivity::processEvents() {
  while (true) {
    SyncEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (eventOverflow_) {
        eventOverflow_ = false;
        eventHead_ = 0;
        eventCount_ = 0;
        hasOverflow = true;
      }
      if (eventCount_ > 0) {
        event = events_[eventHead_];
        eventHead_ = static_cast<uint8_t>((eventHead_ + 1) % MAX_SYNC_EVENTS);
        eventCount_--;
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }

    if (hasOverflow) {
      setError(tr(STR_NEARBY_POSITION_QUEUE_OVERFLOW));
      return;
    }
    if (!hasEvent) return;
    handleEvent(event);
  }
}

void NearbyBookPositionSyncActivity::handleEvent(const SyncEvent& event) {
  if (state_ == State::ERROR || state_ == State::SYNCED) return;

  if (event.type == PacketType::INVALID) {
    setError(tr(STR_NEARBY_POSITION_VERSION_MISMATCH));
    return;
  }

  if (event.type == PacketType::NAME) {
    if (event.deviceMac == peerDeviceMac_ || isZeroMac(peerDeviceMac_)) {
      peerSourceMac_ = event.sourceMac;
      peerDeviceMac_ = event.deviceMac;
      peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
      peerName_ = event.deviceName.data();
      if (sourceMode_) {
        peerSeen_ = true;
        addPeer(peerSourceMac_.data());
        if (!localPositionSent_) sendLocalPosition();
        if (state_ == State::DISCOVERING) setState(State::SYNCING);
      }
      requestUpdate();
    }
    return;
  }

  if (event.type != PacketType::ACK &&
      std::strcmp(event.position.documentHash.data(), localPosition_.documentHash.data()) != 0) {
    setError(tr(STR_NEARBY_POSITION_BOOK_MISMATCH));
    return;
  }

  const bool startingPassiveSync = state_ != State::DISCOVERING && state_ != State::SYNCING &&
                                   state_ != State::APPLYING && state_ != State::SHOWING_RESULT;
  if (startingPassiveSync) {
    errorMessage_.clear();
    peerPositionReceived_ = false;
    sourceMode_ = false;
    localPositionSent_ = false;
    localPositionAcked_ = false;
    syncStartedMs_ = millis();
    lastHelloMs_ = syncStartedMs_;
    lastPositionSendMs_ = 0;
  }

  peerSeen_ = true;
  if (event.deviceMac != peerDeviceMac_) {
    peerName_.clear();
  }
  peerSourceMac_ = event.sourceMac;
  peerDeviceMac_ = event.deviceMac;
  peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
  addPeer(peerSourceMac_.data());

  if (event.type == PacketType::HELLO) {
    sendDeviceName(peerSourceMac_.data());
    if (!sourceMode_) setState(State::SYNCING);
    return;
  }

  if (event.type == PacketType::POSITION) {
    if (sourceMode_) return;
    peerPosition_ = event.position;
    if (!mapPeerPosition()) return;
    peerPositionReceived_ = true;
    sendAck(peerSourceMac_.data());
    setState(State::SHOWING_RESULT);
    return;
  }

  if (event.type == PacketType::APPLY) {
    return;
  }

  if (event.type == PacketType::ACK) {
    if (sourceMode_) {
      localPositionAcked_ = true;
      setState(State::SYNCED);
    } else {
      localPositionAcked_ = true;
    }
  }
}

bool NearbyBookPositionSyncActivity::addPeer(const uint8_t* peerMac) {
  if (!peerMac) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, ESP_NOW_ETH_ALEN);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  const esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

bool NearbyBookPositionSyncActivity::sendPacket(const PacketType type, const uint8_t* peerMac) {
  if (!peerMac || !espNowStarted_) return false;
  if (!addPeer(peerMac)) return false;

  std::array<uint8_t, MAX_PACKET_BYTES> packet = {};
  packet[0] = 'C';
  packet[1] = 'I';
  packet[2] = 'B';
  packet[3] = 'P';
  packet[4] = PROTOCOL_VERSION;
  packet[5] = static_cast<uint8_t>(type);
  std::copy(localDeviceMac_.begin(), localDeviceMac_.end(), packet.begin() + 8);

  uint8_t* payload = packet.data() + PACKET_HEADER_BYTES;
  size_t payloadLength = 0;
  if (type == PacketType::HELLO) {
    if (!writeDocumentHash(payload, localPosition_.documentHash)) return false;
    payloadLength = DOCUMENT_HASH_BYTES;
  } else if (type == PacketType::POSITION || type == PacketType::APPLY) {
    if (!serializePosition(localPosition_, payload, packet.size() - PACKET_HEADER_BYTES, payloadLength)) return false;
  } else if (type == PacketType::NAME) {
    const char* name = SETTINGS.getEffectiveDeviceName();
    payloadLength = std::min(std::strlen(name), MAX_DEVICE_NAME_BYTES);
    if (payloadLength < CrossPointSettings::MIN_DEVICE_NAME_LENGTH) return false;
    memcpy(payload, name, payloadLength);
  }

  packet[6] = payloadLength & 0xFF;
  packet[7] = (payloadLength >> 8) & 0xFF;

  const size_t length = PACKET_HEADER_BYTES + payloadLength;
  const esp_err_t result = esp_now_send(peerMac, packet.data(), length);
  if (result != ESP_OK) {
    LOG_ERR(LOG_TAG, "esp_now_send failed: %d", static_cast<int>(result));
    return false;
  }
  return true;
}

bool NearbyBookPositionSyncActivity::sendHello() {
  lastHelloMs_ = millis();
  return sendPacket(PacketType::HELLO, BROADCAST_MAC);
}

bool NearbyBookPositionSyncActivity::sendDeviceName(const uint8_t* peerMac) {
  return sendPacket(PacketType::NAME, peerMac);
}

bool NearbyBookPositionSyncActivity::sendLocalPosition() {
  if (!peerSeen_) return false;
  sendDeviceName(peerSourceMac_.data());
  const bool sent = sendPacket(PacketType::POSITION, peerSourceMac_.data());
  lastPositionSendMs_ = millis();
  localPositionSent_ = sent;
  return sent;
}

bool NearbyBookPositionSyncActivity::sendAck(const uint8_t* peerMac) { return sendPacket(PacketType::ACK, peerMac); }

bool NearbyBookPositionSyncActivity::mapPeerPosition() {
  if (!ensureEpubLoaded()) {
    setError(tr(STR_SYNC_FAILED_MSG));
    return false;
  }

  KOReaderPosition koPos;
  koPos.xpath = peerPosition_.xpath.data();
  koPos.percentage = qToPercentage(peerPosition_.percentageQ);
  peerCrossPoint_ = ProgressMapper::toCrossPoint(epub_, koPos, currentSpineIndex_, totalPagesInSpine_);
  if (peerCrossPoint_.totalPages <= 0) {
    peerCrossPoint_.spineIndex = peerPosition_.spineIndex;
    peerCrossPoint_.pageNumber = peerPosition_.pageNumber;
    peerCrossPoint_.totalPages = std::max<uint16_t>(1, peerPosition_.totalPages);
  }
  if (peerPosition_.hasParagraphIndex && !peerCrossPoint_.hasParagraphIndex) {
    peerCrossPoint_.paragraphIndex = peerPosition_.paragraphIndex;
    peerCrossPoint_.hasParagraphIndex = true;
  }

  if (peerCrossPoint_.hasLiIndex || peerCrossPoint_.xpathAnchorId[0] != '\0' || peerCrossPoint_.hasParagraphIndex) {
    Section tempSection(epub_, peerCrossPoint_.spineIndex, renderer);
    bool refined = false;
    if (peerCrossPoint_.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(peerCrossPoint_.liIndex);
      if (liPage.has_value()) {
        peerCrossPoint_.pageNumber = *liPage;
        refined = true;
      }
    }
    if (!refined && peerCrossPoint_.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(peerCrossPoint_.xpathAnchorId));
      if (anchorPage.has_value()) {
        peerCrossPoint_.pageNumber = *anchorPage;
        refined = true;
      }
    }
    if (!refined && peerCrossPoint_.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(peerCrossPoint_.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(peerCrossPoint_.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = static_cast<int>(*paragraphPage);
        if (nextParagraphPage.has_value() && refinedPage >= static_cast<int>(*nextParagraphPage)) {
          refinedPage = static_cast<int>(*nextParagraphPage) - 1;
        }
        peerCrossPoint_.pageNumber = refinedPage;
      }
    }
  }

  const int pageCount = std::max(peerCrossPoint_.totalPages, peerCrossPoint_.pageNumber + 1);
  peerCrossPoint_.totalPages = std::max(1, pageCount);
  return true;
}

bool NearbyBookPositionSyncActivity::applyPeerPosition() {
  if (!ensureEpubLoaded()) {
    setError(tr(STR_SYNC_FAILED_MSG));
    return false;
  }

  const int pageCount = std::max(peerCrossPoint_.totalPages, peerCrossPoint_.pageNumber + 1);
  if (!EpubReaderUtils::saveProgress(*epub_, peerCrossPoint_.spineIndex, peerCrossPoint_.pageNumber, pageCount)) {
    setError(tr(STR_SAVE_PROGRESS_FAILED));
    return false;
  }
  setState(State::SYNCED);
  return true;
}

void NearbyBookPositionSyncActivity::updateSyncProgress() {
  if (state_ != State::DISCOVERING && state_ != State::SYNCING && state_ != State::APPLYING) return;

  const uint32_t now = millis();
  if (now - syncStartedMs_ > SYNC_TIMEOUT_MS) {
    setError(I18N.get(peerSeen_ ? StrId::STR_NEARBY_POSITION_TIMEOUT : StrId::STR_NEARBY_POSITION_NO_READER));
    return;
  }

  if (state_ == State::APPLYING) {
    return;
  }

  if (peerPositionReceived_) {
    setState(State::SHOWING_RESULT);
    return;
  }

  if (!peerSeen_ && now - lastHelloMs_ >= HELLO_INTERVAL_MS) {
    sendHello();
    return;
  }

  if (sourceMode_ && peerSeen_ && !localPositionAcked_ && now - lastPositionSendMs_ >= POSITION_RETRY_INTERVAL_MS) {
    sendLocalPosition();
  }
}

void NearbyBookPositionSyncActivity::returnToReader() { activityManager.goToReader(epubPath_); }

void NearbyBookPositionSyncActivity::setState(const State state) {
  if (state_ == state) return;
  state_ = state;
  requestUpdate();
}

void NearbyBookPositionSyncActivity::setError(const std::string& error) {
  LOG_ERR(LOG_TAG, "%s", error.c_str());
  errorMessage_ = error;
  setState(State::ERROR);
}

void NearbyBookPositionSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_NEARBY_POSITION_SYNC));

  if (state_ == State::SHOWING_RESULT) {
    renderComparison();
    renderer.displayBuffer();
    return;
  }

  std::string primary;
  std::string detail;
  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::READY:
      primary = tr(STR_NEARBY_POSITION_READY);
      detail = std::string(tr(STR_DEVICE_NAME)) + ": " + SETTINGS.getEffectiveDeviceName();
      break;
    case State::DISCOVERING:
      primary = tr(STR_NEARBY_POSITION_SCANNING);
      break;
    case State::SYNCING:
      primary = I18N.get(sourceMode_ ? StrId::STR_NEARBY_POSITION_SHARING : StrId::STR_NEARBY_POSITION_WAITING);
      detail = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) + ": " +
               (peerName_.empty() ? peerId_ : peerName_);
      break;
    case State::APPLYING:
      primary = tr(STR_NEARBY_POSITION_APPLYING);
      break;
    case State::SYNCED:
      primary = I18N.get(sourceMode_ ? StrId::STR_NEARBY_POSITION_SHARED : StrId::STR_NEARBY_POSITION_SYNCED);
      detail = std::string(I18N.get(peerName_.empty() ? StrId::STR_SYSTEM_DEVICE : StrId::STR_DEVICE_NAME)) + ": " +
               (peerName_.empty() ? peerId_ : peerName_);
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      detail = errorMessage_;
      break;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    renderReady(primary, detail);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEARBY_POSITION_SHARE_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  const int top = screen.y + screen.height / 2 - 40;
  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!detail.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + renderer.getLineHeight(UI_10_FONT_ID) + 8,
                              detail.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}

void NearbyBookPositionSyncActivity::renderReady(const std::string& primary, const std::string& detailPrimary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int y = screen.y + metrics.topPadding + metrics.headerHeight + 70;

  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, primary.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  if (!detailPrimary.empty()) {
    UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, y, detailPrimary.c_str(), true);
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  }
  if (state_ == State::READY) {
    UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, y, tr(STR_NEARBY_POSITION_READY_HINT), true);
  }
}

void NearbyBookPositionSyncActivity::renderComparison() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEARBY_POSITION_FOUND), true, EpdFontFamily::BOLD);

  const int peerTocIndex = epub_ ? epub_->getTocIndexForSpineIndex(peerCrossPoint_.spineIndex) : -1;
  const std::string peerChapter =
      (epub_ && peerTocIndex >= 0)
          ? epub_->getTocItem(peerTocIndex).title
          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(peerCrossPoint_.spineIndex + 1));
  const std::string localChapter = !localChapterName_.empty()
                                       ? localChapterName_
                                       : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex_ + 1));

  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_NEARBY_LABEL), true);
  char peerChapterStr[128];
  snprintf(peerChapterStr, sizeof(peerChapterStr), "  %s", peerChapter.c_str());
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, peerChapterStr);
  char peerPageStr[64];
  snprintf(peerPageStr, sizeof(peerPageStr), tr(STR_PAGE_OVERALL_FORMAT), peerCrossPoint_.pageNumber + 1,
           qToPercentage(peerPosition_.percentageQ) * 100.0f);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, peerPageStr);
  if (!peerName_.empty()) {
    char deviceStr[64];
    snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), peerName_.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
  }

  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
  char localChapterStr[128];
  snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
  char localPageStr[64];
  snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage_ + 1, totalPagesInSpine_,
           qToPercentage(localPosition_.percentageQ) * 100.0f);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

  const int optionY = top + 230;
  const int optionHeight = 30;
  renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
  renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_NEARBY_POSITION),
                    false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

#endif
