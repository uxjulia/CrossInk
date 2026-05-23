#include "NearbyStatsSyncActivity.h"

#ifdef SIMULATOR

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() = default;

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::onExit() { Activity::onExit(); }

void NearbyStatsSyncActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_STATS_SYNC));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NEARBY_STATS_SIMULATOR_UNAVAILABLE), true,
                            EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t*, const uint8_t*, int) {}

void NearbyStatsSyncActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* LOG_TAG = "NSYNC";
constexpr const char* CROSSPOINT_ROOT = "/.crosspoint";
constexpr const char* GLOBAL_STATS_PATH = "/.crosspoint/global_stats.bin";
constexpr const char* SYNCED_STATS_DIR = "/.crosspoint/synced_stats";
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t MIN_STATS_BYTES = 13;
constexpr uint8_t MAX_STATS_BYTES = 17;
constexpr uint8_t PACKET_HEADER_BYTES = 14;
constexpr uint32_t HELLO_INTERVAL_MS = 750;
constexpr uint32_t STATS_RETRY_INTERVAL_MS = 750;
constexpr uint32_t SYNC_TIMEOUT_MS = 12000;
constexpr uint8_t BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

NearbyStatsSyncActivity* activeActivity = nullptr;

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

std::string statsFileNameForDeviceMac(const std::array<uint8_t, 6>& mac) {
  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

std::string syncedStatsPathForDeviceMac(const std::array<uint8_t, 6>& mac) {
  return std::string(SYNCED_STATS_DIR) + "/" + statsFileNameForDeviceMac(mac);
}

bool isValidStatsPayload(const uint8_t* data, const uint8_t size) {
  return (size == MIN_STATS_BYTES && data[0] == 1) || (size == MAX_STATS_BYTES && data[0] == 2);
}

bool ensureSyncedStatsDirectory() {
  return Storage.ensureDirectoryExists(CROSSPOINT_ROOT) && Storage.ensureDirectoryExists(SYNCED_STATS_DIR);
}

bool readSmallFile(const char* path, std::array<uint8_t, MAX_STATS_BYTES>& out, uint8_t& outSize) {
  outSize = 0;
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_STATS_BYTES || fileSize > MAX_STATS_BYTES) {
    file.close();
    return false;
  }

  const int read = file.read(out.data(), fileSize);
  file.close();
  if (read != static_cast<int>(fileSize) || !isValidStatsPayload(out.data(), static_cast<uint8_t>(fileSize)))
    return false;
  outSize = static_cast<uint8_t>(fileSize);
  return true;
}

bool writeSyncedStatsFile(const std::string& path, const uint8_t* data, const uint8_t size) {
  if (!isValidStatsPayload(data, size) || !ensureSyncedStatsDirectory()) return false;

  const std::string tmpPath = path + ".part";
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath, file)) return false;
  const size_t written = file.write(data, size);
  if (written != size) {
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  file.flush();
  if (!file.sync()) {
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!file.close()) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (!activeActivity || !info || !info->src_addr) return;
  activeActivity->enqueueEspNowPacket(info->src_addr, data, length);
}

}  // namespace

NearbyStatsSyncActivity::NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NearbyStatsSync", renderer, mappedInput), eventMutex_(xSemaphoreCreateMutex()) {}

NearbyStatsSyncActivity::~NearbyStatsSyncActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

void NearbyStatsSyncActivity::onEnter() {
  Activity::onEnter();
  setState(State::STARTING);

  if (esp_efuse_mac_get_default(localDeviceMac_.data()) != ESP_OK) {
    setError("Could not read device id");
    return;
  }

  if (!beginEspNow()) {
    setError("Could not start nearby sync");
    return;
  }

  setState(State::READY);
}

void NearbyStatsSyncActivity::onExit() {
  Activity::onExit();
  endEspNow();
}

void NearbyStatsSyncActivity::loop() {
  processEvents();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) &&
      (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR)) {
    startSync();
    return;
  }

  updateSyncProgress();
}

bool NearbyStatsSyncActivity::beginEspNow() {
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

void NearbyStatsSyncActivity::endEspNow() {
  if (activeActivity == this) activeActivity = nullptr;
  if (espNowStarted_) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    espNowStarted_ = false;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

bool NearbyStatsSyncActivity::prepareLocalStats() {
  localStatsReady_ = false;
  if (!ensureSyncedStatsDirectory()) {
    setError("could not create synced stats directory");
    return false;
  }

  // Creating synced_stats is the opt-in signal; mirror local stats only after
  // the user starts or accepts this workflow.
  GlobalReadingStats::load().save();

  if (!readSmallFile(GLOBAL_STATS_PATH, localStats_, localStatsSize_)) {
    setError("local stats unavailable");
    return false;
  }

  localStatsReady_ = true;
  return true;
}

void NearbyStatsSyncActivity::startSync() {
  errorMessage_.clear();
  peerSeen_ = false;
  peerStatsSaved_ = false;
  localStatsSent_ = false;
  localStatsAcked_ = false;
  peerId_.clear();
  syncStartedMs_ = millis();
  lastHelloMs_ = 0;
  lastStatsSendMs_ = 0;

  if (!prepareLocalStats()) return;

  setState(State::DISCOVERING);
  sendHello();
}

void NearbyStatsSyncActivity::enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, const int length) {
  if (!eventMutex_ || !sourceMac || !data || length < PACKET_HEADER_BYTES) return;
  if (data[0] != 'C' || data[1] != 'I' || data[2] != 'S' || data[3] != 'S') return;
  if (data[4] != PROTOCOL_VERSION) return;

  SyncEvent event;
  event.type = static_cast<PacketType>(data[5]);
  event.statsSize = data[6];
  std::copy(sourceMac, sourceMac + event.sourceMac.size(), event.sourceMac.begin());
  std::copy(data + 8, data + 14, event.deviceMac.begin());

  const int expectedLength = PACKET_HEADER_BYTES + (event.type == PacketType::STATS ? event.statsSize : 0);
  if (length != expectedLength) return;
  if (event.type != PacketType::HELLO && event.type != PacketType::STATS && event.type != PacketType::ACK) return;
  if (event.deviceMac == localDeviceMac_) return;
  if (event.type == PacketType::STATS) {
    if (event.statsSize > event.stats.size() || !isValidStatsPayload(data + PACKET_HEADER_BYTES, event.statsSize))
      return;
    std::copy(data + PACKET_HEADER_BYTES, data + PACKET_HEADER_BYTES + event.statsSize, event.stats.begin());
  } else if (event.statsSize != 0) {
    return;
  }

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
}

void NearbyStatsSyncActivity::processEvents() {
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
      setError("sync event queue overflow");
      return;
    }
    if (!hasEvent) return;
    handleEvent(event);
  }
}

void NearbyStatsSyncActivity::handleEvent(const SyncEvent& event) {
  if (state_ == State::ERROR) return;

  const bool startingPassiveSync = state_ != State::DISCOVERING && state_ != State::SYNCING;
  if (startingPassiveSync) {
    errorMessage_.clear();
    peerStatsSaved_ = false;
    localStatsSent_ = false;
    localStatsAcked_ = false;
    localStatsReady_ = false;
    syncStartedMs_ = millis();
    lastHelloMs_ = syncStartedMs_;
    lastStatsSendMs_ = 0;
  }

  peerSeen_ = true;
  peerSourceMac_ = event.sourceMac;
  peerDeviceMac_ = event.deviceMac;
  peerId_ = bytesToHex(peerDeviceMac_.data(), peerDeviceMac_.size());
  addPeer(peerSourceMac_.data());

  if (!localStatsReady_ && !prepareLocalStats()) return;
  if (state_ == State::READY || state_ == State::DISCOVERING || state_ == State::SYNCED) setState(State::SYNCING);

  if (event.type == PacketType::HELLO) {
    sendLocalStats();
    return;
  }

  if (event.type == PacketType::STATS) {
    if (!writeSyncedStatsFile(syncedStatsPathForDeviceMac(peerDeviceMac_), event.stats.data(), event.statsSize)) {
      setError("could not save stats");
      return;
    }
    peerStatsSaved_ = true;
    sendAck(peerSourceMac_.data());
    if (!localStatsSent_ || !localStatsAcked_) sendLocalStats();
    return;
  }

  if (event.type == PacketType::ACK) {
    localStatsAcked_ = true;
  }
}

bool NearbyStatsSyncActivity::addPeer(const uint8_t* peerMac) {
  if (!peerMac) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, ESP_NOW_ETH_ALEN);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  const esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

bool NearbyStatsSyncActivity::sendPacket(const PacketType type, const uint8_t* peerMac) {
  if (!peerMac || !espNowStarted_) return false;
  if (!addPeer(peerMac)) return false;

  std::array<uint8_t, PACKET_HEADER_BYTES + MAX_STATS_BYTES> packet = {};
  packet[0] = 'C';
  packet[1] = 'I';
  packet[2] = 'S';
  packet[3] = 'S';
  packet[4] = PROTOCOL_VERSION;
  packet[5] = static_cast<uint8_t>(type);
  packet[6] = type == PacketType::STATS ? localStatsSize_ : 0;
  packet[7] = 0;
  std::copy(localDeviceMac_.begin(), localDeviceMac_.end(), packet.begin() + 8);

  size_t length = PACKET_HEADER_BYTES;
  if (type == PacketType::STATS) {
    if (!localStatsReady_ || !isValidStatsPayload(localStats_.data(), localStatsSize_)) return false;
    std::copy(localStats_.begin(), localStats_.begin() + localStatsSize_, packet.begin() + PACKET_HEADER_BYTES);
    length += localStatsSize_;
  }

  const esp_err_t result = esp_now_send(peerMac, packet.data(), length);
  if (result != ESP_OK) {
    LOG_ERR(LOG_TAG, "esp_now_send failed: %d", static_cast<int>(result));
    return false;
  }
  return true;
}

bool NearbyStatsSyncActivity::sendHello() {
  lastHelloMs_ = millis();
  return sendPacket(PacketType::HELLO, BROADCAST_MAC);
}

bool NearbyStatsSyncActivity::sendLocalStats() {
  if (!peerSeen_) return false;
  lastStatsSendMs_ = millis();
  localStatsSent_ = sendPacket(PacketType::STATS, peerSourceMac_.data());
  return localStatsSent_;
}

bool NearbyStatsSyncActivity::sendAck(const uint8_t* peerMac) { return sendPacket(PacketType::ACK, peerMac); }

void NearbyStatsSyncActivity::updateSyncProgress() {
  if (state_ != State::DISCOVERING && state_ != State::SYNCING) return;

  const uint32_t now = millis();
  if (now - syncStartedMs_ > SYNC_TIMEOUT_MS) {
    setError(peerSeen_ ? "stats sync timed out" : "no reader found");
    return;
  }

  if (peerStatsSaved_ && localStatsAcked_) {
    setState(State::SYNCED);
    return;
  }

  if (!peerSeen_ && now - lastHelloMs_ >= HELLO_INTERVAL_MS) {
    sendHello();
    return;
  }

  if (peerSeen_ && localStatsReady_ && !localStatsAcked_ && now - lastStatsSendMs_ >= STATS_RETRY_INTERVAL_MS) {
    sendLocalStats();
  }
}

void NearbyStatsSyncActivity::setState(const State state) {
  if (state_ == state) return;
  state_ = state;
  requestUpdate();
}

void NearbyStatsSyncActivity::setError(const std::string& error) {
  LOG_ERR(LOG_TAG, "%s", error.c_str());
  errorMessage_ = error;
  setState(State::ERROR);
}

void NearbyStatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_STATS_SYNC));

  const int centerY = pageHeight / 2 - 20;
  std::string primary;
  std::string secondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::READY:
      primary = tr(STR_NEARBY_STATS_READY);
      secondary = statsFileNameForDeviceMac(localDeviceMac_);
      break;
    case State::DISCOVERING:
      primary = tr(STR_NEARBY_STATS_SCANNING);
      break;
    case State::SYNCING:
      primary = tr(STR_NEARBY_STATS_SYNCING);
      secondary = peerId_;
      break;
    case State::SYNCED:
      primary = tr(STR_NEARBY_STATS_SYNCED);
      secondary = peerId_;
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      secondary = errorMessage_;
      break;
  }

  if (state_ == State::READY || state_ == State::SYNCED || state_ == State::ERROR) {
    renderReady(primary, secondary);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEARBY_STATS_SYNC_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY, primary.c_str(), true, EpdFontFamily::BOLD);
  if (!secondary.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + renderer.getLineHeight(UI_10_FONT_ID) + 8, secondary.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void NearbyStatsSyncActivity::renderReady(const std::string& primary, const std::string& secondary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop + 70;

  renderer.drawCenteredText(UI_10_FONT_ID, y, primary.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(SMALL_FONT_ID, y, secondary.c_str(), true);
  y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_NEARBY_STATS_SYNC_BUTTON), true);
}

#endif
