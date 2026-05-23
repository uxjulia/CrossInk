#include "BleTransferActivity.h"

#ifdef SIMULATOR

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

struct BleTransferRuntime {};

BleTransferActivity::BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTransfer", renderer, mappedInput) {}

BleTransferActivity::~BleTransferActivity() = default;

void BleTransferActivity::onEnter() {
  Activity::onEnter();
  setState(State::ERROR);
}

void BleTransferActivity::onExit() { Activity::onExit(); }

void BleTransferActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
}

void BleTransferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_BLE_SIMULATOR_UNAVAILABLE), true,
                            EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BleTransferActivity::enqueueBleConnected() {}
void BleTransferActivity::enqueueBleDisconnected() {}
void BleTransferActivity::enqueueControlWrite(const std::string&) {}
void BleTransferActivity::enqueueDataWrite(const std::string&) {}

void BleTransferActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}

#else

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

#include "MappedInputManager.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* BLE_DEVICE_NAME = "CrossInk Stats Sync";
constexpr const char* BLE_SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* BLE_DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10";
constexpr const char* CROSSPOINT_ROOT = "/.crosspoint";
constexpr const char* GLOBAL_STATS_PATH = "/.crosspoint/global_stats.bin";
constexpr const char* SYNCED_STATS_DIR = "/.crosspoint/synced_stats";
constexpr size_t MIN_BLE_STATS_BYTES = 13;
constexpr size_t MAX_BLE_STATS_BYTES = 17;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES = 160;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MIN = 20;
constexpr size_t BLE_DOWNLOAD_CHUNK_BYTES_MAX = BLE_DOWNLOAD_CHUNK_BYTES;
constexpr size_t MAX_FILENAME_BYTES = 96;
constexpr size_t BLE_PROGRESS_STATUS_INTERVAL_BYTES = 4UL * 1024UL;
constexpr size_t BLE_PROGRESS_DISPLAY_INTERVAL_BYTES = 128UL * 1024UL;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MIN = 20;
constexpr size_t BLE_UPLOAD_ACK_BYTES_MAX = 64UL * 1024UL;
constexpr size_t MAX_QUEUED_BLE_EVENTS = 64;
constexpr size_t MAX_QUEUED_BLE_EVENT_BYTES = 8UL * 1024UL;
constexpr size_t BIN_SUFFIX_LEN = 4;
constexpr uint32_t BLE_PEER_SCAN_MS = 6000;
constexpr uint32_t BLE_PEER_CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t BLE_PEER_STATUS_TIMEOUT_MS = 8000;
constexpr uint32_t BLE_PEER_DOWNLOAD_TIMEOUT_MS = 10000;

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

std::string makeDeviceId() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  return bytesToHex(mac, sizeof(mac));
}

std::string toLowerAscii(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool isHexString(const std::string& value, const size_t length) {
  if (value.length() != length) return false;
  return std::all_of(value.begin(), value.end(), [](const char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

bool isHexSha256(const std::string& value) { return isHexString(value, 64); }

bool endsWithSuffix(const std::string& value, const char* suffix, const size_t suffixLen) {
  if (value.length() < suffixLen) return false;
  return toLowerAscii(value.substr(value.length() - suffixLen)) == suffix;
}

bool isSafeBleFileName(const std::string& value) {
  if (value.empty() || value.length() > MAX_FILENAME_BYTES || value[0] == '.') return false;
  for (const char c : value) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool isSafeDeviceId(const std::string& value) { return isHexString(value, 12); }

std::string statsFileNameForDeviceId(const std::string& deviceId) {
  if (!isSafeDeviceId(deviceId)) return {};
  return "device_" + toLowerAscii(deviceId) + ".bin";
}

bool isSafeBleStatsName(const std::string& value) {
  if (!isSafeBleFileName(value) || value.length() != 23 || value.rfind("device_", 0) != 0 ||
      !endsWithSuffix(value, ".bin", BIN_SUFFIX_LEN)) {
    return false;
  }
  return isSafeDeviceId(value.substr(7, 12));
}

std::string syncedStatsPathForDeviceId(const std::string& deviceId) {
  const std::string fileName = statsFileNameForDeviceId(deviceId);
  if (fileName.empty()) return {};
  return std::string(SYNCED_STATS_DIR) + "/" + fileName;
}

bool isValidStatsPayload(const std::string& data) {
  return (data.size() == MIN_BLE_STATS_BYTES && static_cast<uint8_t>(data[0]) == 1) ||
         (data.size() == MAX_BLE_STATS_BYTES && static_cast<uint8_t>(data[0]) == 2);
}

bool ensureSyncedStatsDirectory() {
  return Storage.ensureDirectoryExists(CROSSPOINT_ROOT) && Storage.ensureDirectoryExists(SYNCED_STATS_DIR);
}

bool readSmallFile(const char* path, std::string& out, const size_t maxBytes) {
  out.clear();
  FsFile file;
  if (!Storage.openFileForRead("BLE", path, file)) return false;
  const size_t fileSize = file.fileSize();
  if (fileSize == 0 || fileSize > maxBytes) {
    file.close();
    return false;
  }
  out.resize(fileSize);
  const int read = file.read(reinterpret_cast<uint8_t*>(&out[0]), fileSize);
  file.close();
  return read == static_cast<int>(fileSize);
}

bool writeSyncedStatsFile(const std::string& path, const std::string& data) {
  if (!isValidStatsPayload(data) || !ensureSyncedStatsDirectory()) return false;

  const std::string tmpPath = path + ".part";
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite("BLE", tmpPath, file)) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  if (written != data.size()) {
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

std::string sha256Hex(const uint8_t* data, const size_t length) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, data, length);
  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  return bytesToHex(digest, sizeof(digest));
}

std::string sha256ToHex(const uint8_t digest[32]) { return bytesToHex(digest, 32); }

std::string stateName(const BleTransferActivity::State state) {
  switch (state) {
    case BleTransferActivity::State::STARTING:
      return "starting";
    case BleTransferActivity::State::ADVERTISING:
      return "advertising";
    case BleTransferActivity::State::CONNECTED:
      return "connected";
    case BleTransferActivity::State::RECEIVING_STATS:
      return "receiving_stats";
    case BleTransferActivity::State::VERIFYING_STATS:
      return "verifying_stats";
    case BleTransferActivity::State::SAVED_STATS:
      return "saved_stats";
    case BleTransferActivity::State::SENDING_STATS:
      return "sending_stats";
    case BleTransferActivity::State::SENT_STATS:
      return "sent_stats";
    case BleTransferActivity::State::SYNC_SCANNING:
      return "sync_scanning";
    case BleTransferActivity::State::SYNC_CONNECTING:
      return "sync_connecting";
    case BleTransferActivity::State::SYNCING_STATS:
      return "syncing_stats";
    case BleTransferActivity::State::SYNCED_STATS:
      return "synced_stats";
    case BleTransferActivity::State::ERROR:
      return "error";
  }
  return "unknown";
}

uint32_t readLe32(const std::string& value) {
  const auto* b = reinterpret_cast<const uint8_t*>(value.data());
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

uint32_t readLe32(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    server->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 120);
    server->setDataLen(connInfo.getConnHandle(), 251);
    activity_.enqueueBleConnected();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { activity_.enqueueBleDisconnected(); }

 private:
  BleTransferActivity& activity_;
};

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.enqueueControlWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit DataCallbacks(BleTransferActivity& activity) : activity_(activity) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    activity_.enqueueDataWrite(characteristic->getValue());
  }

 private:
  BleTransferActivity& activity_;
};

struct PeerStatsDownload {
  std::string data;
  uint32_t expectedSequence = 0;
  uint32_t pendingAck = 0;
  bool hasPendingAck = false;
  bool badFrame = false;
};

bool writeJsonControl(NimBLERemoteCharacteristic* control, JsonDocument& doc) {
  String output;
  serializeJson(doc, output);
  return control && control->writeValue(output.c_str(), output.length(), true);
}

bool readRemoteStatus(NimBLERemoteCharacteristic* status, JsonDocument& doc) {
  if (!status) return false;
  const std::string value = status->readValue();
  return !deserializeJson(doc, value.data(), value.size());
}

bool waitForRemoteState(NimBLERemoteCharacteristic* status, const char* desiredState, std::string& error,
                        const uint32_t timeoutMs = BLE_PEER_STATUS_TIMEOUT_MS) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    JsonDocument doc;
    if (readRemoteStatus(status, doc)) {
      const std::string state = doc["state"] | "";
      if (state == desiredState) return true;
      if (state == "error") {
        error = doc["error"] | "peer error";
        return false;
      }
    }
    delay(100);
  }
  error = "peer timed out";
  return false;
}

bool sendDownloadAck(NimBLERemoteCharacteristic* control, const uint32_t sequence) {
  JsonDocument doc;
  doc["op"] = "get_ack";
  doc["sequence"] = sequence;
  return writeJsonControl(control, doc);
}

bool waitForStatsDownload(NimBLERemoteCharacteristic* control, NimBLERemoteCharacteristic* status,
                          PeerStatsDownload& download, size_t& expectedSize, std::string& error) {
  const uint32_t start = millis();
  expectedSize = 0;
  while (millis() - start < BLE_PEER_DOWNLOAD_TIMEOUT_MS) {
    if (download.badFrame) {
      error = "invalid stats frame";
      return false;
    }
    if (download.hasPendingAck) {
      const uint32_t sequence = download.pendingAck;
      download.hasPendingAck = false;
      if (!sendDownloadAck(control, sequence)) {
        error = "could not ack stats";
        return false;
      }
    }

    JsonDocument doc;
    if (readRemoteStatus(status, doc)) {
      const std::string state = doc["state"] | "";
      if (doc["size"].is<size_t>()) expectedSize = doc["size"].as<size_t>();
      if (expectedSize > 0 && download.data.size() == expectedSize && isValidStatsPayload(download.data)) return true;
      if (state == "sent_stats") return true;
      if (state == "error") {
        error = doc["error"] | "peer error";
        return false;
      }
    }
    delay(50);
  }

  error = "stats download timed out";
  return false;
}

void writeLe32(std::vector<uint8_t>& data, const uint32_t value) {
  data.push_back(static_cast<uint8_t>(value & 0xFF));
  data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

}  // namespace

struct BleTransferRuntime {
  explicit BleTransferRuntime(BleTransferActivity& activity)
      : serverCallbacks(activity), controlCallbacks(activity), dataCallbacks(activity) {}

  NimBLEServer* server = nullptr;
  NimBLEService* service = nullptr;
  NimBLECharacteristic* status = nullptr;
  NimBLECharacteristic* dataOut = nullptr;
  ServerCallbacks serverCallbacks;
  ControlCallbacks controlCallbacks;
  DataCallbacks dataCallbacks;

  bool begin(BleTransferActivity& activity) {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(&serverCallbacks, false);

    service = server->createService(BLE_SERVICE_UUID);
    if (!service) return false;

    auto* control = service->createCharacteristic(BLE_CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    auto* dataIn = service->createCharacteristic(BLE_DATA_IN_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    status = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    dataOut = service->createCharacteristic(BLE_DATA_OUT_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (!control || !dataIn || !status || !dataOut) return false;

    control->setCallbacks(&controlCallbacks);
    dataIn->setCallbacks(&dataCallbacks);
    status->setValue(activity.buildStatusJson());

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setName(BLE_DEVICE_NAME);
    advertising->start();
    return true;
  }

  void publish(const std::string& json) {
    if (!status) return;
    status->setValue(json);
    status->notify();
  }

  void notifyData(const uint8_t* data, const size_t length) {
    if (!dataOut) return;
    dataOut->setValue(data, length);
    dataOut->notify();
  }

  void startAdvertising() {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->start();
  }

  void end() {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    server = nullptr;
    service = nullptr;
    status = nullptr;
    dataOut = nullptr;
  }
};

BleTransferActivity::BleTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleTransfer", renderer, mappedInput), eventMutex_(xSemaphoreCreateMutex()) {}

BleTransferActivity::~BleTransferActivity() {
  if (eventMutex_) {
    vSemaphoreDelete(eventMutex_);
    eventMutex_ = nullptr;
  }
}

void BleTransferActivity::onEnter() {
  Activity::onEnter();
  deviceId_ = makeDeviceId();
  mbedtls_sha256_init(&shaContext_);
  setState(State::STARTING);

  ble_ = std::make_unique<BleTransferRuntime>(*this);
  if (!ble_->begin(*this)) {
    setError("Could not start BLE");
    return;
  }

  setState(State::ADVERTISING);
  publishStatus();
}

void BleTransferActivity::onExit() {
  Activity::onExit();
  resetTransfer(true);
  if (ble_) {
    ble_->end();
    ble_.reset();
  }
  mbedtls_sha256_free(&shaContext_);
}

void BleTransferActivity::loop() {
  processBleEvents();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && state_ == State::ADVERTISING) {
    startPeerStatsSync();
    return;
  }
  if (pendingCommit_) {
    pendingCommit_ = false;
    processCommit();
    return;
  }
  if (state_ == State::SENDING_STATS && downloadOpen_) {
    if (statusDirty_) publishStatus();
    pumpDownload();
    return;
  }
  if (statusDirty_) publishStatus();
}

void BleTransferActivity::enqueueBleEvent(BleEvent event) {
  if (!eventMutex_) return;
  const size_t eventBytes = event.value.size();
  xSemaphoreTake(eventMutex_, portMAX_DELAY);
  if (bleEventOverflow_ || bleEvents_.size() >= MAX_QUEUED_BLE_EVENTS ||
      queuedBleEventBytes_ + eventBytes > MAX_QUEUED_BLE_EVENT_BYTES) {
    bleEventOverflow_ = true;
    queuedBleEventBytes_ = 0;
    bleEvents_.clear();
  } else {
    queuedBleEventBytes_ += eventBytes;
    bleEvents_.push_back(std::move(event));
  }
  xSemaphoreGive(eventMutex_);
}

void BleTransferActivity::enqueueBleConnected() { enqueueBleEvent({BleEventType::CONNECTED, {}}); }

void BleTransferActivity::enqueueBleDisconnected() { enqueueBleEvent({BleEventType::DISCONNECTED, {}}); }

void BleTransferActivity::enqueueControlWrite(const std::string& value) {
  enqueueBleEvent({BleEventType::CONTROL, value});
}

void BleTransferActivity::enqueueDataWrite(const std::string& value) { enqueueBleEvent({BleEventType::DATA, value}); }

void BleTransferActivity::processBleEvents() {
  while (true) {
    BleEvent event;
    bool hasEvent = false;
    bool hasOverflow = false;
    if (eventMutex_) {
      xSemaphoreTake(eventMutex_, portMAX_DELAY);
      if (bleEventOverflow_) {
        bleEventOverflow_ = false;
        queuedBleEventBytes_ = 0;
        bleEvents_.clear();
        hasOverflow = true;
      }
      if (!bleEvents_.empty()) {
        event = std::move(bleEvents_.front());
        queuedBleEventBytes_ -= event.value.size();
        bleEvents_.pop_front();
        hasEvent = true;
      }
      xSemaphoreGive(eventMutex_);
    }
    if (hasOverflow) {
      resetTransfer(true);
      setError("BLE event queue overflow");
      return;
    }
    if (!hasEvent) return;

    switch (event.type) {
      case BleEventType::CONNECTED:
        onBleConnected();
        break;
      case BleEventType::DISCONNECTED:
        onBleDisconnected();
        break;
      case BleEventType::CONTROL:
        onControlWrite(event.value);
        break;
      case BleEventType::DATA:
        onDataWrite(event.value);
        break;
    }
  }
}

void BleTransferActivity::onBleConnected() {
  helloAccepted_ = false;
  setState(State::CONNECTED);
}

void BleTransferActivity::onBleDisconnected() {
  if (state_ == State::SAVED_STATS || state_ == State::SENT_STATS || state_ == State::SYNCED_STATS) {
    resetTransfer(false);
    helloAccepted_ = false;
    setState(State::SYNCED_STATS);
    if (ble_) ble_->startAdvertising();
    return;
  }

  if (transferOpen_ || downloadOpen_) {
    resetTransfer(true);
    setError("reader disconnected");
    return;
  }
  helloAccepted_ = false;
  setState(State::ADVERTISING);
  if (ble_) ble_->startAdvertising();
}

void BleTransferActivity::onControlWrite(const std::string& value) {
  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, value.data(), value.size());
  if (parseError) {
    setError("invalid control JSON");
    return;
  }

  const std::string op = doc["op"] | "";
  if (op == "peer_hello") {
    const int version = doc["version"] | 0;
    const std::string peerDeviceId = toLowerAscii(doc["device_id"] | "");
    if (version != 1 || !isSafeDeviceId(peerDeviceId) || peerDeviceId == deviceId_) {
      setError("invalid peer hello");
      return;
    }
    helloAccepted_ = true;
    setState(State::CONNECTED);
    return;
  }

  if (!helloAccepted_) {
    setError("peer hello required");
    return;
  }

  if (op == "start_put") {
    resetTransfer(true);

    const std::string kind = doc["kind"] | "";
    fileName_ = doc["name"] | "";
    expectedSize_ = doc["size"] | 0;
    expectedSha256_ = toLowerAscii(doc["sha256"] | "");
    uploadAckBytes_ = doc["ack_bytes"] | BLE_UPLOAD_ACK_BYTES_MIN;

    if (kind != "stats") {
      setError("unsupported transfer kind");
      return;
    }
    if (!isSafeBleStatsName(fileName_)) {
      setError("unsafe stats filename");
      return;
    }
    if (fileName_ == statsFileNameForDeviceId(deviceId_)) {
      setError("refusing local stats overwrite");
      return;
    }
    if (expectedSize_ < MIN_BLE_STATS_BYTES || expectedSize_ > MAX_BLE_STATS_BYTES) {
      setError("invalid stats size");
      return;
    }
    if (!isHexSha256(expectedSha256_)) {
      setError("invalid sha256");
      return;
    }
    if (uploadAckBytes_ < BLE_UPLOAD_ACK_BYTES_MIN || uploadAckBytes_ > BLE_UPLOAD_ACK_BYTES_MAX) {
      setError("invalid ack window");
      return;
    }
    if (!ensureSyncedStatsDirectory()) {
      setError("could not create synced stats directory");
      return;
    }

    partPath_ = std::string(SYNCED_STATS_DIR) + "/.ble-" + fileName_ + ".part";
    finalPath_ = std::string(SYNCED_STATS_DIR) + "/" + fileName_;
    if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
    if (!Storage.openFileForWrite("BLE", partPath_, uploadFile_)) {
      setError("could not open stats file");
      return;
    }

    mbedtls_sha256_starts(&shaContext_, 0);
    shaActive_ = true;
    receivedBytes_ = 0;
    expectedSequence_ = 0;
    transferOpen_ = true;
    removePartOnExit_ = true;
    lastProgressStatusBytes_ = 0;
    lastDisplayProgressBytes_ = 0;
    setState(State::RECEIVING_STATS);
    return;
  }

  if (op == "start_get") {
    resetTransfer(true);
    const std::string kind = doc["kind"] | "";
    const int64_t offsetValue = doc["offset"] | 0;
    const int64_t chunkSizeValue = doc["chunk_size"] | static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES);
    if (kind != "stats") {
      setError("unsupported transfer kind");
      return;
    }
    if (offsetValue < 0 ||
        static_cast<uint64_t>(offsetValue) > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      setError("invalid download offset");
      return;
    }
    if (chunkSizeValue < static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MIN) ||
        chunkSizeValue > static_cast<int64_t>(BLE_DOWNLOAD_CHUNK_BYTES_MAX)) {
      setError("invalid download chunk size");
      return;
    }
    startStatsDownload(static_cast<size_t>(offsetValue), static_cast<size_t>(chunkSizeValue));
    return;
  }

  if (op == "get_ack") {
    if (!downloadOpen_ || !downloadAwaitingAck_) {
      setError("no download pending");
      return;
    }
    const uint32_t sequence = doc["sequence"] | UINT32_MAX;
    if (sequence != pendingDownloadAck_) {
      setError("unexpected download ack");
      return;
    }
    downloadAwaitingAck_ = false;
    statusDirty_ = true;
    requestUpdate();
    return;
  }

  if (op == "commit") {
    if (!transferOpen_) {
      setError("no transfer open");
      return;
    }
    pendingCommit_ = true;
    setState(State::VERIFYING_STATS);
    return;
  }

  if (op == "cancel") {
    resetTransfer(true);
    setState(State::CONNECTED);
    return;
  }

  setError("unknown control op");
}

void BleTransferActivity::onDataWrite(const std::string& value) {
  if (!transferOpen_ || state_ != State::RECEIVING_STATS) return;
  if (value.size() <= sizeof(uint32_t)) {
    setError("invalid data frame");
    resetTransfer(true);
    return;
  }

  const uint32_t sequence = readLe32(value);
  if (sequence != expectedSequence_) {
    setError("unexpected data sequence");
    resetTransfer(true);
    return;
  }

  const uint8_t* payload = reinterpret_cast<const uint8_t*>(value.data() + sizeof(uint32_t));
  const size_t payloadSize = value.size() - sizeof(uint32_t);
  if (receivedBytes_ + payloadSize > expectedSize_) {
    setError("transfer too large");
    resetTransfer(true);
    return;
  }
  if (uploadFile_.write(payload, payloadSize) != payloadSize) {
    setError("transfer write failed");
    resetTransfer(true);
    return;
  }

  mbedtls_sha256_update(&shaContext_, payload, payloadSize);
  receivedBytes_ += payloadSize;
  expectedSequence_++;
  if (receivedBytes_ == expectedSize_ || receivedBytes_ - lastProgressStatusBytes_ >= uploadAckBytes_) {
    lastProgressStatusBytes_ = receivedBytes_;
    statusDirty_ = true;
  }
  if (receivedBytes_ == expectedSize_ ||
      receivedBytes_ - lastDisplayProgressBytes_ >= BLE_PROGRESS_DISPLAY_INTERVAL_BYTES) {
    lastDisplayProgressBytes_ = receivedBytes_;
    requestUpdate();
  }
}

void BleTransferActivity::processCommit() {
  if (!transferOpen_) return;
  setState(State::VERIFYING_STATS);

  uploadFile_.flush();
  uploadFile_.close();
  transferOpen_ = false;

  if (receivedBytes_ != expectedSize_) {
    setError("size mismatch");
    resetTransfer(true);
    return;
  }

  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&shaContext_, digest);
  shaActive_ = false;
  if (sha256ToHex(digest) != expectedSha256_) {
    setError("sha256 mismatch");
    resetTransfer(true);
    return;
  }

  std::string statsData;
  if (!readSmallFile(partPath_.c_str(), statsData, MAX_BLE_STATS_BYTES) || !isValidStatsPayload(statsData)) {
    setError("invalid stats file");
    resetTransfer(true);
    return;
  }
  if (Storage.exists(finalPath_.c_str()) && !Storage.remove(finalPath_.c_str())) {
    setError("could not replace stats file");
    resetTransfer(true);
    return;
  }
  if (!Storage.rename(partPath_.c_str(), finalPath_.c_str())) {
    setError("could not finalize stats");
    resetTransfer(true);
    return;
  }

  removePartOnExit_ = false;
  savedPath_ = finalPath_;
  setState(State::SAVED_STATS);
}

void BleTransferActivity::startStatsDownload(const size_t offset, const size_t chunkSize) {
  GlobalReadingStats::load().save();

  if (!Storage.exists(GLOBAL_STATS_PATH)) {
    setError("not_found");
    return;
  }
  if (!Storage.openFileForRead("BLE", GLOBAL_STATS_PATH, downloadFile_)) {
    setError("could not open stats");
    return;
  }

  const size_t fileSize = downloadFile_.fileSize();
  if (fileSize < MIN_BLE_STATS_BYTES || fileSize > MAX_BLE_STATS_BYTES) {
    downloadFile_.close();
    setError("invalid stats size");
    return;
  }
  if (offset > fileSize) {
    downloadFile_.close();
    setError("invalid download offset");
    return;
  }
  if (offset < fileSize && offset % chunkSize != 0) {
    downloadFile_.close();
    setError("unaligned download offset");
    return;
  }
  if (!downloadFile_.seek(offset)) {
    downloadFile_.close();
    setError("could not seek stats");
    return;
  }

  fileName_ = statsFileNameForDeviceId(deviceId_);
  expectedSize_ = fileSize;
  sentBytes_ = offset;
  downloadSequence_ = static_cast<uint32_t>(offset / chunkSize);
  pendingDownloadAck_ = 0;
  downloadAwaitingAck_ = false;
  downloadChunkSize_ = chunkSize;
  lastProgressStatusBytes_ = sentBytes_;
  downloadOpen_ = true;
  setState(State::SENDING_STATS);
}

void BleTransferActivity::pumpDownload() {
  if (!downloadOpen_ || downloadAwaitingAck_) return;

  std::array<uint8_t, sizeof(uint32_t) + BLE_DOWNLOAD_CHUNK_BYTES> frame = {};
  frame[0] = static_cast<uint8_t>(downloadSequence_ & 0xFF);
  frame[1] = static_cast<uint8_t>((downloadSequence_ >> 8) & 0xFF);
  frame[2] = static_cast<uint8_t>((downloadSequence_ >> 16) & 0xFF);
  frame[3] = static_cast<uint8_t>((downloadSequence_ >> 24) & 0xFF);

  const int read = downloadFile_.read(frame.data() + sizeof(uint32_t), downloadChunkSize_);
  if (read < 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setError("download read failed");
    return;
  }
  if (read == 0) {
    downloadFile_.close();
    downloadOpen_ = false;
    setState(State::SENT_STATS);
    return;
  }

  ble_->notifyData(frame.data(), sizeof(uint32_t) + static_cast<size_t>(read));
  sentBytes_ += static_cast<size_t>(read);
  pendingDownloadAck_ = downloadSequence_;
  downloadAwaitingAck_ = true;
  downloadSequence_++;
  if (sentBytes_ == expectedSize_ || sentBytes_ - lastProgressStatusBytes_ >= BLE_PROGRESS_STATUS_INTERVAL_BYTES) {
    lastProgressStatusBytes_ = sentBytes_;
    statusDirty_ = true;
    requestUpdate();
  }
}

void BleTransferActivity::startPeerStatsSync() {
  resetTransfer(true);
  setState(State::SYNC_SCANNING);
  publishStatus();
  requestUpdateAndWait();

  if (!ensureSyncedStatsDirectory()) {
    setError("could not create synced stats directory");
    return;
  }

  // Creating synced_stats is the opt-in signal; mirror local stats only after
  // the user starts this workflow.
  GlobalReadingStats::load().save();

  std::string localStats;
  if (!readSmallFile(GLOBAL_STATS_PATH, localStats, MAX_BLE_STATS_BYTES) || !isValidStatsPayload(localStats)) {
    setError("local stats unavailable");
    return;
  }

  if (ble_) NimBLEDevice::stopAdvertising();

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) {
    setError("could not scan");
    if (ble_) ble_->startAdvertising();
    return;
  }
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);

  const NimBLEUUID serviceUuid(BLE_SERVICE_UUID);
  NimBLEScanResults results = scan->getResults(BLE_PEER_SCAN_MS, false);
  const NimBLEAdvertisedDevice* peer = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* candidate = results.getDevice(i);
    if (candidate && candidate->isAdvertisingService(serviceUuid)) {
      peer = candidate;
      break;
    }
  }

  if (!peer) {
    scan->clearResults();
    if (ble_) ble_->startAdvertising();
    setError("no reader found");
    return;
  }

  setState(State::SYNC_CONNECTING);
  publishStatus();
  requestUpdateAndWait();

  std::string error;
  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) {
    scan->clearResults();
    if (ble_) ble_->startAdvertising();
    setError("could not create client");
    return;
  }
  client->setConnectionParams(12, 12, 0, 150);
  client->setConnectTimeout(BLE_PEER_CONNECT_TIMEOUT_MS);

  auto finishClient = [&]() {
    if (client) {
      if (client->isConnected()) client->disconnect();
      NimBLEDevice::deleteClient(client);
      client = nullptr;
    }
    scan->clearResults();
    if (ble_) ble_->startAdvertising();
  };

  if (!client->connect(peer)) {
    finishClient();
    setError("could not connect reader");
    return;
  }
  client->setDataLen(251);

  NimBLERemoteService* service = client->getService(BLE_SERVICE_UUID);
  NimBLERemoteCharacteristic* control = service ? service->getCharacteristic(BLE_CONTROL_UUID) : nullptr;
  NimBLERemoteCharacteristic* dataIn = service ? service->getCharacteristic(BLE_DATA_IN_UUID) : nullptr;
  NimBLERemoteCharacteristic* status = service ? service->getCharacteristic(BLE_STATUS_UUID) : nullptr;
  NimBLERemoteCharacteristic* dataOut = service ? service->getCharacteristic(BLE_DATA_OUT_UUID) : nullptr;
  if (!service || !control || !dataIn || !status || !dataOut) {
    finishClient();
    setError("reader service incomplete");
    return;
  }

  JsonDocument statusDoc;
  if (!readRemoteStatus(status, statusDoc)) {
    finishClient();
    setError("could not read reader status");
    return;
  }
  const std::string peerDeviceId = toLowerAscii(statusDoc["device_id"] | "");
  if (!isSafeDeviceId(peerDeviceId) || peerDeviceId == deviceId_) {
    finishClient();
    setError("invalid reader id");
    return;
  }

  setState(State::SYNCING_STATS);
  publishStatus();
  requestUpdateAndWait();

  JsonDocument hello;
  hello["op"] = "peer_hello";
  hello["version"] = 1;
  hello["device_id"] = deviceId_.c_str();
  if (!writeJsonControl(control, hello) || !waitForRemoteState(status, "connected", error)) {
    finishClient();
    setError(error.empty() ? "reader hello failed" : error);
    return;
  }

  const std::string localStatsName = statsFileNameForDeviceId(deviceId_);
  const std::string localStatsSha = sha256Hex(reinterpret_cast<const uint8_t*>(localStats.data()), localStats.size());
  JsonDocument startPut;
  startPut["op"] = "start_put";
  startPut["kind"] = "stats";
  startPut["name"] = localStatsName.c_str();
  startPut["size"] = localStats.size();
  startPut["sha256"] = localStatsSha.c_str();
  startPut["ack_bytes"] = BLE_UPLOAD_ACK_BYTES_MIN;
  if (!writeJsonControl(control, startPut) || !waitForRemoteState(status, "receiving_stats", error)) {
    finishClient();
    setError(error.empty() ? "stats upload failed" : error);
    return;
  }

  std::vector<uint8_t> frame;
  frame.reserve(sizeof(uint32_t) + localStats.size());
  writeLe32(frame, 0);
  frame.insert(frame.end(), localStats.begin(), localStats.end());
  if (!dataIn->writeValue(frame.data(), frame.size(), true)) {
    finishClient();
    setError("could not send stats");
    return;
  }

  JsonDocument commit;
  commit["op"] = "commit";
  if (!writeJsonControl(control, commit) || !waitForRemoteState(status, "saved_stats", error)) {
    finishClient();
    setError(error.empty() ? "stats upload commit failed" : error);
    return;
  }

  PeerStatsDownload download;
  auto notifyCb = [&download](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    if (length <= sizeof(uint32_t)) {
      download.badFrame = true;
      return;
    }
    const uint32_t sequence = readLe32(data);
    if (sequence != download.expectedSequence || download.hasPendingAck) {
      download.badFrame = true;
      return;
    }
    download.data.append(reinterpret_cast<const char*>(data + sizeof(uint32_t)), length - sizeof(uint32_t));
    download.pendingAck = sequence;
    download.hasPendingAck = true;
    download.expectedSequence++;
  };
  if (!dataOut->subscribe(true, notifyCb)) {
    finishClient();
    setError("could not receive stats");
    return;
  }

  JsonDocument startGet;
  startGet["op"] = "start_get";
  startGet["kind"] = "stats";
  startGet["offset"] = 0;
  startGet["chunk_size"] = BLE_DOWNLOAD_CHUNK_BYTES;
  if (!writeJsonControl(control, startGet)) {
    dataOut->unsubscribe();
    finishClient();
    setError("could not request stats");
    return;
  }

  size_t expectedDownloadSize = 0;
  if (!waitForStatsDownload(control, status, download, expectedDownloadSize, error)) {
    dataOut->unsubscribe();
    finishClient();
    setError(error);
    return;
  }
  dataOut->unsubscribe();

  if (expectedDownloadSize != download.data.size() || !isValidStatsPayload(download.data)) {
    finishClient();
    setError("invalid received stats");
    return;
  }

  const std::string peerStatsPath = syncedStatsPathForDeviceId(peerDeviceId);
  if (peerStatsPath.empty() || !writeSyncedStatsFile(peerStatsPath, download.data)) {
    finishClient();
    setError("could not save stats");
    return;
  }

  finishClient();
  setState(State::SYNCED_STATS);
}

void BleTransferActivity::resetTransfer(const bool removePart) {
  if (shaActive_) {
    mbedtls_sha256_free(&shaContext_);
    mbedtls_sha256_init(&shaContext_);
    shaActive_ = false;
  }
  if (uploadFile_) uploadFile_.close();
  if (downloadFile_) downloadFile_.close();
  if (removePart && removePartOnExit_ && !partPath_.empty() && Storage.exists(partPath_.c_str())) {
    Storage.remove(partPath_.c_str());
  }

  fileName_.clear();
  partPath_.clear();
  finalPath_.clear();
  expectedSha256_.clear();
  savedPath_.clear();
  expectedSize_ = 0;
  receivedBytes_ = 0;
  sentBytes_ = 0;
  lastProgressStatusBytes_ = 0;
  lastDisplayProgressBytes_ = 0;
  uploadAckBytes_ = BLE_UPLOAD_ACK_BYTES_MIN;
  downloadChunkSize_ = BLE_DOWNLOAD_CHUNK_BYTES;
  expectedSequence_ = 0;
  downloadSequence_ = 0;
  pendingDownloadAck_ = 0;
  transferOpen_ = false;
  downloadOpen_ = false;
  downloadAwaitingAck_ = false;
  pendingCommit_ = false;
  removePartOnExit_ = false;
}

void BleTransferActivity::setState(const State state) {
  state_ = state;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::setError(const std::string& error) {
  errorMessage_ = error;
  state_ = State::ERROR;
  statusDirty_ = true;
  requestUpdate();
}

void BleTransferActivity::publishStatus() {
  statusDirty_ = false;
  if (ble_) ble_->publish(buildStatusJson());
}

std::string BleTransferActivity::buildStatusJson() const {
  JsonDocument doc;
  const std::string state = stateName(state_);
  doc["state"] = state.c_str();
  doc["protocol_version"] = 1;
  doc["sync_kind"] = "stats";
  doc["device_id"] = deviceId_.c_str();
  JsonArray uploadKinds = doc["upload_kinds"].to<JsonArray>();
  uploadKinds.add("stats");
  JsonArray downloadKinds = doc["download_kinds"].to<JsonArray>();
  downloadKinds.add("stats");
  if (expectedSize_ > 0 || state_ == State::SENDING_STATS || state_ == State::SENT_STATS) {
    doc["kind"] = "stats";
    if (state_ == State::SENDING_STATS || state_ == State::SENT_STATS) {
      doc["sent"] = sentBytes_;
    } else {
      doc["received"] = receivedBytes_;
      doc["ack_bytes"] = uploadAckBytes_;
    }
    doc["size"] = expectedSize_;
  }
  if (state_ == State::SAVED_STATS && !savedPath_.empty()) {
    doc["name"] = fileName_.c_str();
    doc["path"] = savedPath_.c_str();
  }
  if (state_ == State::SENT_STATS) doc["name"] = fileName_.c_str();
  if (state_ == State::ERROR && !errorMessage_.empty()) doc["error"] = errorMessage_.c_str();

  String output;
  serializeJson(doc, output);
  return output.c_str();
}

void BleTransferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH_TRANSFER));

  const int centerY = pageHeight / 2 - 20;
  std::string primary;
  std::string secondary;

  switch (state_) {
    case State::STARTING:
      primary = tr(STR_LOADING_POPUP);
      break;
    case State::ADVERTISING:
      primary = tr(STR_BLE_TRANSFER_READY);
      secondary = statsFileNameForDeviceId(deviceId_);
      break;
    case State::CONNECTED:
      primary = tr(STR_CONNECTED);
      break;
    case State::RECEIVING_STATS:
    case State::SENDING_STATS: {
      primary = tr(STR_BLE_STATS_SYNCING);
      char buffer[48];
      const size_t progress = state_ == State::RECEIVING_STATS ? receivedBytes_ : sentBytes_;
      snprintf(buffer, sizeof(buffer), "%u / %u bytes", static_cast<unsigned>(progress),
               static_cast<unsigned>(expectedSize_));
      secondary = buffer;
      break;
    }
    case State::VERIFYING_STATS:
      primary = tr(STR_BLE_STATS_SYNCING);
      secondary = fileName_;
      break;
    case State::SAVED_STATS:
      primary = tr(STR_BLE_STATS_SYNCED);
      secondary = savedPath_.empty() ? fileName_ : savedPath_;
      break;
    case State::SENT_STATS:
      primary = tr(STR_BLE_STATS_SYNCED);
      secondary = fileName_;
      break;
    case State::SYNC_SCANNING:
      primary = tr(STR_BLE_STATS_SCANNING);
      break;
    case State::SYNC_CONNECTING:
      primary = tr(STR_BLE_STATS_CONNECTING);
      break;
    case State::SYNCING_STATS:
      primary = tr(STR_BLE_STATS_SYNCING);
      break;
    case State::SYNCED_STATS:
      primary = tr(STR_BLE_STATS_SYNCED);
      break;
    case State::ERROR:
      primary = tr(STR_ERROR_MSG);
      secondary = errorMessage_;
      break;
  }

  if (state_ == State::ADVERTISING || state_ == State::CONNECTED) {
    renderReady(primary, secondary);
    const char* syncLabel = state_ == State::ADVERTISING ? tr(STR_BLE_SYNC_STATS) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), syncLabel, "", "");
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

void BleTransferActivity::renderReady(const std::string& primary, const std::string& secondary) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = contentTop + 70;

  renderer.drawCenteredText(UI_10_FONT_ID, y, primary.c_str(), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(SMALL_FONT_ID, y, secondary.c_str(), true);
  y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_BLE_SYNC_STATS), true);
}

#endif
