#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <DeviceSecret.h>
#include <FontFileTypes.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "util/FirmwareIdentity.h"
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <Epub.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "ProtectedPaths.h"
#include "QuickActions.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "components/HeaderDate.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/LogoPng.generated.h"
#include "html/PluginHostJs.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/StyleCss.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncPending.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"
#include "util/FontFamilyLabel.h"
#include "util/OpdsDownloadFolder.h"
#include "util/ReaderStatusBarJson.h"
#include "util/StringUtils.h"

namespace {
// Arduino's WebServer retains parsed request arguments until the next request.
// For JSON POSTs, that includes the complete "plain" body. Expose a narrowly
// scoped release operation so large request bodies do not remain resident
// between requests, and so outbound TLS can reuse that memory immediately.
class CrossPointHttpServer final : public WebServer {
 public:
  explicit CrossPointHttpServer(uint16_t port) : WebServer(port) {}

  void releaseRequestArguments() {
#ifndef SIMULATOR
    if (_currentArgs) {
      delete[] _currentArgs;
      _currentArgs = nullptr;
    }
    _currentArgCount = 0;

    if (_postArgs) {
      delete[] _postArgs;
      _postArgs = nullptr;
    }
    _postArgsLen = 0;
#endif
  }
};

// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled.
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

bool isTwoFingerSwipeSetting(const SettingInfo& setting) {
  return setting.nameId == StrId::STR_TWO_FINGER_SWIPE_UP || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
         setting.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT;
}

bool isSwipeActionSetting(const SettingInfo& setting) {
  return isTwoFingerSwipeSetting(setting) || setting.nameId == StrId::STR_LEFT_EDGE_UP ||
         setting.nameId == StrId::STR_LEFT_EDGE_DOWN || setting.nameId == StrId::STR_RIGHT_EDGE_UP ||
         setting.nameId == StrId::STR_RIGHT_EDGE_DOWN;
}

bool isWebEnumOptionAvailable(const SettingInfo& setting, size_t optionIndex) {
  if (optionIndex >= setting.enumValues.size()) return true;

  const StrId option = setting.enumValues[optionIndex];
  if (option == StrId::STR_THEME_COVER_GRID && !UITheme::supportsCoverGrid()) return false;
  if (!SETTINGS.shouldTrackReadingStats()) {
    if (option == StrId::STR_READING_STATS) return false;
    if (setting.valuePtr == &CrossPointSettings::sleepScreen && optionIndex < setting.enumRawValues.size()) {
      const uint8_t raw = setting.enumRawValues[optionIndex];
      if (raw == CrossPointSettings::READING_STATS_SLEEP || raw == CrossPointSettings::MINIMAL_STATS_SLEEP)
        return false;
    }
  }
  if (option == StrId::STR_TOGGLE_TOUCHSCREEN && !gpio.hasTouch()) return false;

  if (!Frontlight.present()) {
    if (option == StrId::STR_TOGGLE_FRONTLIGHT ||
        (isSwipeActionSetting(setting) &&
         (option == StrId::STR_INCREASE_BRIGHTNESS || option == StrId::STR_DECREASE_BRIGHTNESS ||
          option == StrId::STR_INCREASE_WARMTH || option == StrId::STR_DECREASE_WARMTH))) {
      return false;
    }
    return setting.nameId != StrId::STR_REFRESH_FREQ || optionIndex >= setting.enumRawValues.size() ||
           setting.enumRawValues[optionIndex] != CrossPointSettings::REFRESH_NEVER;
  }

  return Frontlight.hasColorTemperature() || !isSwipeActionSetting(setting) ||
         (option != StrId::STR_INCREASE_WARMTH && option != StrId::STR_DECREASE_WARMTH);
}

uint8_t enumDisplayIndexForWeb(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) return rawValue;

  uint8_t displayIndex = 0;
  for (size_t optionIndex = 0; optionIndex < setting.enumRawValues.size(); ++optionIndex) {
    if (!isWebEnumOptionAvailable(setting, optionIndex)) continue;
    if (setting.enumRawValues[optionIndex] == rawValue) return displayIndex;
    ++displayIndex;
  }
  return 0;
}

bool isWebSettingAvailable(const SettingInfo& setting) {
  if (isTiltPageTurnSetting(setting) && !QuickActions::supportsTiltPageTurn()) return false;

  if (setting.category == StrId::STR_STATUS_BARS || setting.nameId == StrId::STR_HIDE_CLOCK ||
      settingKeyIs(setting, "displayStatusBarTextSize")) {
    return false;
  }
  if (setting.nameId == StrId::STR_SIDE_BUTTON_CHORD && !deviceSupportsSideButtonChord(gpio)) {
    return false;
  }

  if (setting.nameId == StrId::STR_MENU_NAVIGATION && !deviceHasFrontButtons()) {
    return false;
  }

  const bool isTouchSetting =
      setting.nameId == StrId::STR_DISABLE_TOUCHSCREEN || setting.nameId == StrId::STR_NEXT_PAGE ||
      setting.nameId == StrId::STR_PREV_PAGE || setting.nameId == StrId::STR_TAP_HIDE_STATUS_BAR ||
      setting.nameId == StrId::STR_PINCH_FONT_RESIZE || setting.nameId == StrId::STR_TWO_FINGER_ROTATION ||
      setting.nameId == StrId::STR_TWO_FINGER_SWIPE_UP || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
      setting.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT ||
      setting.nameId == StrId::STR_LEFT_EDGE_UP || setting.nameId == StrId::STR_LEFT_EDGE_DOWN ||
      setting.nameId == StrId::STR_RIGHT_EDGE_UP || setting.nameId == StrId::STR_RIGHT_EDGE_DOWN;
  if (isTouchSetting && !gpio.hasTouch()) {
    return false;
  }

  const bool isMultiTouchSetting = setting.nameId == StrId::STR_PINCH_FONT_RESIZE ||
                                   setting.nameId == StrId::STR_TWO_FINGER_ROTATION || isTwoFingerSwipeSetting(setting);
  if (isMultiTouchSetting && !gpio.supportsMultiTouch()) {
    return false;
  }

  const bool isFrontlightSetting = setting.nameId == StrId::STR_BRIGHTNESS || setting.nameId == StrId::STR_WARMTH ||
                                   setting.nameId == StrId::STR_FRONTLIGHT ||
                                   setting.nameId == StrId::STR_RESTORE_LIGHT_ON_WAKE ||
                                   setting.nameId == StrId::STR_FRONTLIGHT_SCHEDULE ||
                                   setting.nameId == StrId::STR_START || setting.nameId == StrId::STR_END;
  if (isFrontlightSetting && !Frontlight.present()) {
    return false;
  }

  const bool isFrontlightScheduleSetting = setting.nameId == StrId::STR_FRONTLIGHT_SCHEDULE ||
                                           setting.nameId == StrId::STR_START || setting.nameId == StrId::STR_END;
  if (isFrontlightScheduleSetting && !halClock.isAvailable()) {
    return false;
  }

  if (!halClock.isAvailable()) {
    switch (setting.nameId) {
      case StrId::STR_HIDE_CLOCK:
      case StrId::STR_AUTO_BACKUP_STATS:
      case StrId::STR_CLOCK_UTC_OFFSET:
      case StrId::STR_CLOCK_FORMAT:
      case StrId::STR_DATE_FORMAT:
      case StrId::STR_DATE_SEPARATOR:
      case StrId::STR_CLOCK_SYNCED:
        return false;
      default:
        break;
    }
  }

  return true;
}

// Streams catalog JSON responses in bounded pieces. This avoids holding
// both an ArduinoJson document and its serialized String in the fragmented
// network heap, and gives WiFi a chance to drain each piece before the next.
class WebJsonWriter {
 public:
  explicit WebJsonWriter(WebServer& server) : server_(server) {}

  void append(const char* text) { append(text, strlen(text)); }

  void append(const char* text, size_t textLength) {
    while (textLength > 0) {
      if (length_ == sizeof(buffer_)) flush();
      const size_t copyLength = std::min(textLength, sizeof(buffer_) - length_);
      memcpy(buffer_ + length_, text, copyLength);
      length_ += copyLength;
      text += copyLength;
      textLength -= copyLength;
    }
  }

  // ArduinoJson writer interface: serialize without a full-size output copy.
  size_t write(const uint8_t value) {
    append(reinterpret_cast<const char*>(&value), 1);
    return 1;
  }

  size_t write(const uint8_t* data, const size_t length) {
    append(reinterpret_cast<const char*>(data), length);
    return length;
  }

  void appendUnsigned(unsigned long value) {
    char number[16];
    const int length = snprintf(number, sizeof(number), "%lu", value);
    append(number, static_cast<size_t>(length));
  }

  void appendJsonString(const char* value) {
    append("\"");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
      switch (*p) {
        case '\"':
          append("\\\"");
          break;
        case '\\':
          append("\\\\");
          break;
        case '\b':
          append("\\b");
          break;
        case '\f':
          append("\\f");
          break;
        case '\n':
          append("\\n");
          break;
        case '\r':
          append("\\r");
          break;
        case '\t':
          append("\\t");
          break;
        default:
          if (*p < 0x20) {
            static constexpr char kHexDigits[] = "0123456789ABCDEF";
            const char escaped[] = {'\\', 'u', '0', '0', kHexDigits[*p >> 4], kHexDigits[*p & 0x0F]};
            append(escaped, sizeof(escaped));
          } else {
            append(reinterpret_cast<const char*>(p), 1);
          }
          break;
      }
    }
    append("\"");
  }

  void flush() {
    if (length_ == 0) return;
    server_.sendContent(buffer_, length_);
    length_ = 0;
  }

 private:
  static constexpr size_t BUFFER_SIZE = 192;
  WebServer& server_;
  char buffer_[BUFFER_SIZE];
  size_t length_ = 0;
};

// WebSocket upload state
HalFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedPath(const String& path) {
  if (protectedpaths::isSensitivePath(path.c_str())) return true;
  // Hidden/system items stay out of the default file-manager view. Enabling
  // Show Hidden Files intentionally makes them fully manageable.
  if (SETTINGS.showHiddenFiles) return false;

  // Check every segment so a hidden parent also protects its descendants.
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();

    String segment = path.substring(start, end);

    if (segment.startsWith(".")) return true;

    for (const auto* item : HIDDEN_ITEMS) {
      if (segment.equals(item)) return true;
    }

    start = end + 1;
  }

  return false;
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server = makeUniqueNoThrow<CrossPointHttpServer>(port);
  if (!server) {
    LOG_ERR("WEB", "OOM: web server");
    return;
  }

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Add Access-Control-Allow-* headers to every response so web-based clients
  // and PWAs on other origins can use the HTTP API. Preflight OPTIONS requests
  // are answered in handleNotFound().
  server->enableCORS(true);

  // Setup routes
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
  server->on("/style.css", HTTP_GET, [this] { handleStyleCss(); });
  server->on("/logo.png", HTTP_GET, [this] { handleLogo(); });

  server->on("/plugin-host.js", HTTP_GET, [this] {
    server->sendHeader("Content-Encoding", "gzip");
    server->send_P(200, "application/javascript", PluginHostJs, PluginHostJsCompressedSize);
  });
  server->on("/api/plugins", HTTP_GET, [this] { handlePluginList(); });
  server->on("/plugin", HTTP_GET, [this] { handlePluginFile(); });
  server->on("/plugins-run", HTTP_GET, [this] { handlePluginRunnerPage(); });
  server->on("/api/plugin-jobs", HTTP_POST, [this] { handlePluginJobSubmit(); });
  server->on("/api/plugin-jobs/claim", HTTP_GET, [this] { handlePluginJobClaim(); });
  server->on("/api/plugin-jobs/complete", HTTP_POST, [this] { handlePluginJobComplete(); });
  server->on("/api/plugin-jobs/status", HTTP_GET, [this] { handlePluginJobStatus(); });
  server->on("/api/relay", HTTP_POST, [this] { handleRelay(); });
  server->on("/api/crypto", HTTP_POST, [this] { handleCrypto(); });
  server->on("/api/fetch", HTTP_POST, [this] { handleFetch(); });
  server->on("/api/book-key", HTTP_POST, [this] { handleBookKey(); });
  server->on("/api/plugin-fs", HTTP_POST, [this] { handlePluginFs(); }, [this] { handlePluginFsUpload(); });
  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  server->on("/api/status-bars", HTTP_GET, [this] { handleGetStatusBars(); });
  server->on("/api/status-bars", HTTP_POST, [this] { handlePostStatusBars(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout", "If-None-Match"};
  server->collectHeaders(davHeaders, 7);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped

  server->begin();

  // Start WebSocket server for fast binary uploads
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  // Do not subscribe the serving task to the task watchdog. Arduino WebServer
  // permits five-second client and ACK waits, which can consume the entire
  // default watchdog window on a weak connection even while the CPU idle task
  // is healthy. The system idle-task watchdog still detects real CPU stalls.

  running = true;

  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

bool CrossPointWebServer::dropUploadIfCancelled() const {
  if (!uploadCancelCheck || !uploadCancelCheck(uploadCancelContext)) return false;
  server->client().stop();
  return true;
}

void CrossPointWebServer::abortUpload(UploadState& state, const char* error) const {
  state.success = false;
  state.bufferPos = 0;
  if (state.file) {
    state.file.close();
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;
    if (!Storage.remove(filePath.c_str())) {
      LOG_ERR("WEB", "Could not remove incomplete upload: %s", filePath.c_str());
    }
  }
  state.error = error;
  LOG_DBG("WEB", "%s", error);
}

void CrossPointWebServer::abortFontUpload() {
  fontUpload.bufferPos = 0;
  if (fontUpload.file) fontUpload.file.close();
  if (!fontUpload.filePath.empty()) Storage.remove(fontUpload.filePath.c_str());
  fontUpload.valid = false;
  LOG_DBG("WEB", "Font upload aborted");
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  if (pluginFsUpload.file.isOpen()) pluginFsUpload.file.close();
  if (!pluginFsUpload.tmp.empty()) Storage.remove(pluginFsUpload.tmp.c_str());
  pluginFsUpload = PluginFsUploadState{};
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();

  // Brief delay before deletion
  delay(10);

  server.reset();

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendStaticContent(WebServer* server, const char* data, size_t len, const char* etag,
                              const char* contentType = "text/html") {
  server->sendHeader("ETag", etag);
  // Revalidate on each visit so firmware updates cannot leave stale pages cached.
  server->sendHeader("Cache-Control", "no-cache");
  if (server->header("If-None-Match") == etag) {
    server->send(304);
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, contentType, data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendStaticContent(server.get(), HomePageHtml, sizeof(HomePageHtml), HomePageHtmlETag);
}

void CrossPointWebServer::handleJszip() const {
  sendStaticContent(server.get(), jszip_minJs, jszip_minJsCompressedSize, jszip_minJsETag, "application/javascript");
}

// Shared stylesheet and logo are referenced with a content-hashed ?v= query,
// so they can be cached aggressively: a new build changes the URL.
void CrossPointWebServer::handleStyleCss() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "text/css", StyleCss, StyleCssCompressedSize);
}

void CrossPointWebServer::handleLogo() const {
  // Raw PNG (already compressed); no Content-Encoding.
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "image/png", LogoPng, LogoPngSize);
}

void CrossPointWebServer::handleNotFound() const {
  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = AppVersion::version();
  doc["firmwareFamily"] = firmwareidentity::FAMILY;
  doc["pluginApi"] = firmwareidentity::PLUGIN_API_VERSION;
  doc["pluginCapabilities"] = firmwareidentity::PLUGIN_CAPABILITIES;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  // ?plugin=<name> adds a stable device ID for that plugin: sha256(secret ||
  // name). Not reversible to any hardware ID, and different per plugin.
  std::string plugin = server->arg("plugin").c_str();
  for (char& c : plugin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  uint8_t secret[32];
  if (SETTINGS.pluginsEnabled && !plugin.empty() && plugin.length() <= 64 && deviceSecret(secret)) {
    uint8_t input[32 + 64];
    memcpy(input, secret, sizeof(secret));
    memcpy(input + sizeof(secret), plugin.c_str(), plugin.length());
    uint8_t hash[32];
    if (wc_Sha256Hash(input, sizeof(secret) + plugin.length(), hash) == 0) {
      char hex[65];
      for (size_t i = 0; i < sizeof(hash); i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
      doc["deviceId"] = hex;
    }
  }

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
#else
#ifdef SIMULATOR
  doc["device"] = "Simulator";
#else
  doc["device"] = BoardConfig::ACTIVE.name;
#endif
#endif

  char snBuf[33] = {0};
  bool valid = false;
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32's efuse table has no USER_DATA block (C3/S3 only)
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }
#endif

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

bool CrossPointWebServer::scanFiles(const char* path, const FileVisitor visitor, void* context) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return false;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return false;
  }

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    if (visitor) {
      file.getName(name, sizeof(name));
      auto fileName = String(name);

      // Skip hidden items (starting with ".")
      bool shouldHide = (!SETTINGS.showHiddenFiles && fileName.startsWith(".")) ||
                        protectedpaths::isSensitivePath((String(path) + "/" + fileName).c_str());

      // Treat OS/device metadata like other hidden items: keep it out of the
      // default view, but let users manage it when Show Hidden Files is enabled.
      if (!shouldHide && !SETTINGS.showHiddenFiles) {
        for (const auto* item : HIDDEN_ITEMS) {
          if (fileName.equals(item)) {
            shouldHide = true;
            break;
          }
        }
      }

      if (!shouldHide) {
        FileInfo info;
        info.name = fileName;
        info.isDirectory = file.isDirectory();

        if (info.isDirectory) {
          info.size = 0;
          info.isEpub = false;
        } else {
          info.size = file.size();
          info.isEpub = isEpubFile(info.name);
        }

        visitor(info, context);
      }
    }

    file.close();
    yield();  // Yield to allow WiFi and other tasks to process during long scans
    file = root.openNextFile();
  }
  const bool complete = !FsHelpers::directoryIterationFailed(root);
  if (!complete) {
    LOG_ERR("WEB", "Directory listing failed before EOF: %s", path);
  }
  root.close();
  return complete;
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendStaticContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml), FilesPageHtmlETag);
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  // This response runs on the web-server task, so a TCP-sized heap buffer is
  // safer than adding 1.4KB to its stack. Allocation is fallible and retains
  // the old per-entry path as a low-memory fallback.
  constexpr size_t BATCH_CAPACITY = 1400;
  auto batch = makeUniqueNoThrow<char[]>(BATCH_CAPACITY);
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  // Check the iterator before committing to a streamed 200 response. Without
  // this pass, an SD read error is indistinguishable from a complete JSON list.
  if (!scanFiles(currentPath.c_str(), nullptr, nullptr)) {
    server->send(500, "application/json", "{\"error\":\"Directory listing failed\"}");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");

  struct FileListContext {
    WebServer* server;
    char* batch;
    size_t batchLen;
    char* output;
    JsonDocument* doc;
    bool seenFirst;
  } context{server.get(), batch.get(), 0, output, &doc, false};

  if (batch) {
    batch[context.batchLen++] = '[';
  } else {
    LOG_ERR("WEB", "OOM: file list batch buffer; falling back to per-entry sends");
    server->sendContent("[");
  }

  const bool complete = scanFiles(
      currentPath.c_str(),
      [](const FileInfo& info, void* rawContext) {
        auto& context = *static_cast<FileListContext*>(rawContext);
        context.doc->clear();
        (*context.doc)["name"] = info.name;
        (*context.doc)["size"] = info.size;
        (*context.doc)["isDirectory"] = info.isDirectory;
        (*context.doc)["isEpub"] = info.isEpub;

        const size_t written = serializeJson(*context.doc, context.output, outputSize);
        if (written >= outputSize) {
          // JSON output truncated; skip this entry to avoid sending malformed JSON
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }

        const size_t required = written + (context.seenFirst ? 1 : 0);
        if (context.batch) {
          if (context.batchLen + required > BATCH_CAPACITY) {
            context.server->sendContent(context.batch, context.batchLen);
            context.batchLen = 0;
          }
          if (context.seenFirst) context.batch[context.batchLen++] = ',';
          memcpy(context.batch + context.batchLen, context.output, written);
          context.batchLen += written;
        } else {
          if (context.seenFirst) context.server->sendContent(",");
          context.server->sendContent(context.output);
        }
        context.seenFirst = true;
      },
      &context);

  if (!complete) {
    // A second-pass failure must not be closed into a valid partial array.
    server->client().stop();
    return;
  }

  if (batch) {
    if (context.batchLen + 1 > BATCH_CAPACITY) {
      server->sendContent(batch.get(), context.batchLen);
      context.batchLen = 0;
    }
    batch[context.batchLen++] = ']';
    server->sendContent(batch.get(), context.batchLen);
  } else {
    server->sendContent("]");
  }
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
#ifndef SIMULATOR
  client.clear();
#endif
  file.close();
}

// Upload start time is used for the completion throughput summary.
static unsigned long uploadStartTime = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    state.fileName = StringUtils::sanitizeFilename(upload.filename.c_str()).c_str();
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    state.bufferPos = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Access denied to protected path";
      LOG_DBG("WEB", "[UPLOAD] FAILED: Access denied to protected path: %s", filePath.c_str());
      return;
    }

    // Check if file already exists.
    if (Storage.exists(filePath.c_str())) {
      state.error = "File already exists: " + state.fileName;
      LOG_DBG("WEB", "[UPLOAD] Collision: %s", filePath.c_str());
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (dropUploadIfCancelled()) {
      abortUpload(state);
      return;
    }
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            LOG_ERR("WEB", "Failed to flush upload buffer");
            abortUpload(state, "Failed to write to SD card - disk may be full");
            return;
          }
        }
      }

      state.size += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    // The final body chunk can produce END even after cancellation.
    if (dropUploadIfCancelled()) {
      abortUpload(state);
      return;
    }
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state) || !state.file.sync()) {
        LOG_ERR("WEB", "Failed to finish upload");
        abortUpload(state, "Failed to write final data to SD card");
        return;
      }
      if (!state.file.close()) {
        state.error = "Failed to close file on SD card";
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        if (!Storage.remove(filePath.c_str())) {
          LOG_ERR("WEB", "Could not remove incomplete upload: %s", filePath.c_str());
        }
        LOG_ERR("WEB", "%s", state.error.c_str());
        return;
      }

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);

        // Clear epub cache after uploading the file
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        if (!clearBookCachePreservingUserState(filePath.c_str(), true)) {
          state.success = false;
          state.error = "File saved, but could not refresh cached reading/sync data";
          LOG_ERR("WEB", "%s: %s", state.error.c_str(), filePath.c_str());
          return;
        }
        ImageFolderIndex::invalidateForPath(filePath.c_str());
        sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    abortUpload(state);
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String requestedName = server->arg("name");
  size_t leadingDots = 0;
  while (leadingDots < requestedName.length() && requestedName[leadingDots] == '.') {
    leadingDots++;
  }

  const String nameSuffix = requestedName.substring(leadingDots);
  bool suffixHasNameCharacter = false;
  for (size_t i = 0; i < nameSuffix.length(); ++i) {
    if (nameSuffix[i] != ' ' && nameSuffix[i] != '.') {
      suffixHasNameCharacter = true;
      break;
    }
  }
  if (!suffixHasNameCharacter || leadingDots >= StringUtils::kDefaultMaxFilenameBytes) {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }
  const size_t suffixBudget =
      StringUtils::kDefaultMaxFilenameBytes > leadingDots ? StringUtils::kDefaultMaxFilenameBytes - leadingDots : 0;
  const String sanitizedSuffix = StringUtils::sanitizeFilename(nameSuffix.c_str(), suffixBudget).c_str();
  String folderName = requestedName.substring(0, leadingDots);
  folderName += sanitizedSuffix;

  // Validate folder name
  if (folderName.isEmpty() || folderName == "book") {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  HalFile parent = Storage.open(parentPath.c_str());
  if (!parent || !parent.isDirectory()) {
    parent.close();
    server->send(404, "text/plain", "Parent directory does not exist");
    return;
  }
  parent.close();

  if (isProtectedPath(parentPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  // Allow creating a new hidden folder in a visible parent. Existing hidden
  // and system-managed paths remain protected unless Show Hidden Files is on.
  if (isProtectedPath(folderPath) && !folderName.startsWith(".")) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    const auto visibility = FsHelpers::directoryEntryVisibility(parentPath.c_str(), folderPath.c_str());
    if (visibility != FsHelpers::DirectoryEntryVisibility::Visible) {
      const bool rolledBack =
          visibility == FsHelpers::DirectoryEntryVisibility::Missing && Storage.rmdir(folderPath.c_str());
      LOG_ERR("WEB", "Created folder is not enumerable: %s (visibility=%u rollback=%d)", folderPath.c_str(),
              static_cast<unsigned>(visibility), rolledBack);
      server->send(500, "text/plain", "Folder could not be added to the directory listing");
      return;
    }
    ImageFolderIndex::invalidateForPath(folderPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  // Calculate new path to check if it's protected
  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  // Release the validation handle before migrating metadata and renaming the
  // book; real SD cards cannot open the same path through multiple readers.
  file.close();
  const auto migration = BookMoveUtils::renameFilePreservingBookState(itemPath.c_str(), newPath.c_str());
  if (migration == BookMoveUtils::RenameMigrationResult::InvalidBookType) {
    server->send(400, "text/plain", "Renaming a book cannot change its file type");
    return;
  }
  if (migration == BookMoveUtils::RenameMigrationResult::DestinationStateExists) {
    server->send(409, "text/plain", "Target filename has saved reading data. Choose another filename.");
    return;
  }
  if (migration == BookMoveUtils::RenameMigrationResult::RolledBack) {
    LOG_ERR("WEB", "Failed to rename file while preserving reader state: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Could not rename file while preserving saved reading data");
    return;
  }

  LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
  ImageFolderIndex::invalidateForPath(itemPath.c_str());
  sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
  ImageFolderIndex::invalidateForPath(newPath.c_str());
  sdFontSystem.markRegistryDirtyForPath(newPath.c_str());
  if (migration == BookMoveUtils::RenameMigrationResult::KeepRenamed) {
    LOG_ERR("WEB", "Rename kept new path after incomplete state rollback: %s", newPath.c_str());
    server->send(500, "text/plain",
                 "File was renamed, but some saved references could not be updated. Refresh the file list.");
    return;
  }
  server->send(200, "text/plain", "Renamed successfully");
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    if (!clearBookCachePreservingUserState(newPath.c_str(), true)) {
      LOG_ERR("WEB", "File moved but cache refresh failed: %s", newPath.c_str());
      server->send(500, "text/plain", "File moved, but could not refresh cached reading/sync data");
      return;
    }
    ImageFolderIndex::invalidateForPath(itemPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    ImageFolderIndex::invalidateForPath(newPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    if (isProtectedPath(itemPath) || protectedpaths::isSensitivePath(itemPath.c_str(), true)) {
      failedItems += itemPath + " (protected path); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      f.close();
      success = Storage.removeDir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
      if (success) {
        SyncPending::removePath(std::string(itemPath.c_str()));
        BookSyncFile::onDeleted(std::string(itemPath.c_str()));
      }
    }

    if (!success) {
      LOG_ERR("WEB", "Failed to delete item: %s", itemPath.c_str());
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    } else {
      ImageFolderIndex::invalidateForPath(itemPath.c_str());
      sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendStaticContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml), SettingsPageHtmlETag);
}

void CrossPointWebServer::handleGetStatusBars() const {
  JsonDocument doc;
  writeReaderStatusBarJson(doc["top"].to<JsonObject>(), SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top));
  writeReaderStatusBarJson(doc["bottom"].to<JsonObject>(), SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom));
  doc["xtcMode"] = SETTINGS.xtcStatusBarMode;
  doc["displayTextSize"] = SETTINGS.displayStatusBarTextSize;
  doc["clockAvailable"] = halClock.isAvailable();
  JsonArray displaySlots = doc["display"].to<JsonArray>();
  for (const auto item : SETTINGS.displayStatusBar.slots) displaySlots.add(static_cast<uint8_t>(item));
  doc["displayBatteryStyle"] = static_cast<uint8_t>(SETTINGS.displayStatusBar.batteryStyle);

  JsonObject labels = doc["labels"].to<JsonObject>();
  labels["display"] = tr(STR_STATUS_BAR);
  labels["top"] = tr(STR_TOP_STATUS_BAR);
  labels["bottom"] = tr(STR_BOTTOM_STATUS_BAR);
  labels["left"] = tr(STR_DIR_LEFT);
  labels["center"] = tr(STR_CENTER);
  labels["right"] = tr(STR_DIR_RIGHT);
  labels["battery"] = tr(STR_BATTERY);
  labels["percentageFormat"] = tr(STR_PERCENTAGE_FORMAT);
  labels["progressBar"] = tr(STR_PROGRESS_BAR);
  labels["thickness"] = tr(STR_PROGRESS_BAR_THICKNESS);
  labels["hidden"] = tr(STR_HIDE);
  labels["centered"] = tr(STR_CENTER_STATUS_BAR);
  labels["separator"] = tr(STR_SEPARATOR);
  labels["textSize"] = tr(STR_STATUS_BAR_TEXT_SIZE);
  labels["xtcMode"] = tr(STR_XTC_STATUS_BAR);
  labels["preview"] = tr(STR_PREVIEW);

  JsonArray options = doc["options"].to<JsonArray>();
  const auto addOption = [&options](ReaderStatusBarItem item, const std::string& label) {
    JsonObject option = options.add<JsonObject>();
    option["value"] = static_cast<uint8_t>(item);
    option["label"] = label;
  };
  addOption(ReaderStatusBarItem::Clock, tr(STR_STATUS_BAR_CLOCK));
  addOption(ReaderStatusBarItem::Date, tr(STR_DATE));
  char dateText[32];
  doc["datePreview"] = formatHeaderDateText(dateText, sizeof(dateText)) ? dateText : "";
  addOption(ReaderStatusBarItem::Battery, tr(STR_BATTERY));
  const auto combined = [](const char* first, const char* second) { return std::string(first) + " (" + second + ")"; };
  addOption(ReaderStatusBarItem::TimeLeftBook, combined(tr(STR_TIME_LEFT), tr(STR_BOOK)).c_str());
  addOption(ReaderStatusBarItem::TimeLeftChapter, combined(tr(STR_TIME_LEFT), tr(STR_CHAPTER)).c_str());
  addOption(ReaderStatusBarItem::ChapterPageCount, tr(STR_CHAPTER_PAGE_COUNT));
  addOption(ReaderStatusBarItem::StablePageNumber, tr(STR_STABLE_PAGE_NUMBERS));
  addOption(ReaderStatusBarItem::BookProgressPercentage, tr(STR_BOOK_PROGRESS_PERCENTAGE));
  addOption(ReaderStatusBarItem::ChapterProgressPercentage, tr(STR_CHAPTER_PROGRESS_PERCENTAGE));
  addOption(ReaderStatusBarItem::TitleBook, combined(tr(STR_TITLE), tr(STR_BOOK)).c_str());
  addOption(ReaderStatusBarItem::TitleChapter, combined(tr(STR_TITLE), tr(STR_CHAPTER)).c_str());
  addOption(ReaderStatusBarItem::Empty, tr(STR_STATUS_BAR_EMPTY));

  const auto addLabels = [&doc](const char* name, std::initializer_list<StrId> ids) {
    JsonArray labels = doc[name].to<JsonArray>();
    for (const StrId id : ids) labels.add(I18N.get(id));
  };
  addLabels("batteryStyles",
            {StrId::STR_BATTERY_ICON_AND_PERCENT, StrId::STR_BATTERY_ICON_ONLY, StrId::STR_BATTERY_PERCENT_ONLY});
  addLabels("percentageFormats", {StrId::STR_PERCENTAGE_FORMAT_WHOLE, StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                                  StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS});
  addLabels("progressModes", {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE});
  addLabels("thicknesses",
            {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK});
  addLabels("hideOptions", {StrId::STR_OFF, StrId::STR_ON});
  JsonArray separators = doc["separators"].to<JsonArray>();
  for (uint8_t i = 0; i < static_cast<uint8_t>(ReaderStatusBarSeparator::Count); ++i) {
    separators.add(readerStatusBarSeparatorGlyph(static_cast<ReaderStatusBarSeparator>(i)));
  }
  addLabels("textSizes", {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE});
  addLabels("xtcModes", {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP, StrId::STR_STATUS_BAR_BOTH});

  String payload;
  serializeJson(doc, payload);
  server->send(200, "application/json", payload);
}

void CrossPointWebServer::handlePostStatusBars() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain"))) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  const auto textSize = doc["displayTextSize"];
  if (!textSize.isUnbound() && (!textSize.is<int>() || textSize.as<int>() < 0 || textSize.as<int>() > 2)) {
    server->send(400, "text/plain", "Invalid status bar text size");
    return;
  }
  ReaderStatusBarsPayload bars;
  DisplayStatusBarConfig display;
  {
    std::lock_guard<std::mutex> lock(SETTINGS.getMutex());
    display = SETTINGS.displayStatusBar;
  }
  if ((!doc["display"].isNull() && !readDisplayStatusBarJson(doc["display"], display, halClock.isAvailable())) ||
      !readReaderStatusBarBatteryStyle(doc["displayBatteryStyle"], display.batteryStyle) ||
      !CrossPointSettings::parseReaderStatusBars(doc.as<JsonVariantConst>(), bars)) {
    server->send(400, "text/plain", "Invalid status bar configuration");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(SETTINGS.getMutex());
    const auto& previousTop = SETTINGS.topReaderStatusBar;
    if (previousTop.slots != bars.top.slots || previousTop.percentageFormat != bars.top.percentageFormat ||
        previousTop.progressBar != bars.top.progressBar ||
        previousTop.progressBarThickness != bars.top.progressBarThickness ||
        (!doc["top"]["centered"].isUnbound() && previousTop.centered != bars.top.centered) ||
        (!doc["top"]["separator"].isUnbound() && previousTop.separator != bars.top.separator) ||
        (!doc["top"]["battery"].isUnbound() && previousTop.batteryStyle != bars.top.batteryStyle) ||
        SETTINGS.xtcStatusBarMode != bars.xtcMode) {
      SETTINGS.legacyXtcTopUsesBottom = 0;
    }
    // Older clients have no Hide field; editing slots must retain visibility.
    if (doc["top"]["hidden"].isUnbound()) bars.top.hidden = previousTop.hidden;
    if (doc["bottom"]["hidden"].isUnbound()) bars.bottom.hidden = SETTINGS.bottomReaderStatusBar.hidden;
    if (doc["top"]["centered"].isUnbound()) bars.top.centered = previousTop.centered;
    if (doc["bottom"]["centered"].isUnbound()) bars.bottom.centered = SETTINGS.bottomReaderStatusBar.centered;
    if (doc["top"]["separator"].isUnbound()) bars.top.separator = previousTop.separator;
    if (doc["bottom"]["separator"].isUnbound()) bars.bottom.separator = SETTINGS.bottomReaderStatusBar.separator;
    if (doc["top"]["battery"].isUnbound()) bars.top.batteryStyle = previousTop.batteryStyle;
    if (doc["bottom"]["battery"].isUnbound()) bars.bottom.batteryStyle = SETTINGS.bottomReaderStatusBar.batteryStyle;
    SETTINGS.topReaderStatusBar = bars.top;
    SETTINGS.bottomReaderStatusBar = bars.bottom;
    SETTINGS.xtcStatusBarMode = bars.xtcMode;
    if (!doc["display"].isNull() || !doc["displayBatteryStyle"].isUnbound()) SETTINGS.displayStatusBar = display;
    if (!textSize.isUnbound()) SETTINGS.displayStatusBarTextSize = textSize.as<uint8_t>();
  }
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("WEB", "Failed to save status bar configuration");
    server->send(500, "text/plain", "Failed to save status bars");
    return;
  }
  server->send(200, "text/plain", "Status bars saved");
}

void CrossPointWebServer::handleGetSettings() const {
  // The device settings UI needs an owned, mutable copy of the settings list.
  // The web API only reads it, so iterate the static base list directly rather
  // than copying its nested vectors and callbacks while WiFi is using the heap.
  sdFontSystem.refreshIfDirty();
  const auto& settings = getBaseSettingsList();
  const auto& fontFamilies = sdFontSystem.registry().getFamilies();
  const SdCardFontFamilyInfo* selectedSdFamily =
      SETTINGS.sdFontFamilyName[0] == '\0' ? nullptr : sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  WebJsonWriter output(*server);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;  // Skip ACTION-only and unavailable entries.

    doc.clear();
    doc["key"] = s.key;
    if (isSideButtonActionSetting(s)) {
      const bool up = settingKeyIs(s, "sideButtonUpShort") || settingKeyIs(s, "sideButtonUpLong");
      doc["name"] = sideButtonGroupLabel(up) + " " + I18N.get(s.nameId);
    } else {
      doc["name"] = I18N.get(s.nameId);
    }
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr == &CrossPointSettings::shortPwrBtn || s.valuePtr == &CrossPointSettings::longPwrBtn) {
          doc["footnotesIndex"] = enumDisplayIndexForWeb(s, CrossPointSettings::FOOTNOTES);
        } else if (s.valuePtr == &CrossPointSettings::longPressMenuAction ||
                   s.valuePtr == &CrossPointSettings::longPressBackAction) {
          doc["footnotesIndex"] = enumDisplayIndexForWeb(s, CrossPointSettings::LONG_MENU_FOOTNOTES);
        }
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          uint8_t selected = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
          if (selectedSdFamily) {
            const auto it = std::find_if(
                fontFamilies.begin(), fontFamilies.end(),
                [](const SdCardFontFamilyInfo& family) { return family.name == SETTINGS.sdFontFamilyName; });
            if (it != fontFamilies.end()) {
              selected = static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT +
                                              std::distance(fontFamilies.begin(), it));
            }
          }
          doc["value"] = selected;
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily && s.valuePtr) {
          const auto sizes = selectedSdFamily->availableSizes();
          const uint8_t selectedPointSize = SETTINGS.*(s.valuePtr);
          const auto it = std::find(sizes.begin(), sizes.end(), selectedPointSize);
          doc["value"] = static_cast<int>(it == sizes.end() ? 0 : std::distance(sizes.begin(), it));
        } else if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForWeb(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          constexpr auto builtinRange = BUILTIN_FONT_POINT_SIZE_RANGE;
          options.add(fontFamilyLabel(I18N.get(StrId::STR_LEXEND_DECA), builtinRange));
          options.add(fontFamilyLabel(I18N.get(StrId::STR_BITTER), builtinRange));
          for (const auto& family : fontFamilies) {
            options.add(fontFamilyLabel(family.name, fontFamilyPointSizeRange(family)));
          }
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily) {
          const auto sizes = selectedSdFamily->availableSizes();
          for (const uint8_t pointSize : sizes) {
            char label[8];
            snprintf(label, sizeof(label), "%u pt", pointSize);
            options.add(label);
          }
        } else if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (size_t optionIndex = 0; optionIndex < s.enumValues.size(); ++optionIndex) {
            if (isWebEnumOptionAvailable(s, optionIndex)) {
              options.add(sideButtonOptionLabel(s, static_cast<uint8_t>(optionIndex)));
            }
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.value16Ptr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.value16Ptr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        // Passwords are write-only in the web UI. Returning the KOReader
        // value can expose credentials and, for legacy invalid data, emit
        // binary bytes that make the whole JSON response unparsable.
        if (strcmp(s.key, "koPassword") == 0) {
          doc["value"] = "";
        } else if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    if (seenFirst) output.append(",");
    serializeJson(doc, output);
    output.flush();
    seenFirst = true;
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
  sdFontSystem.releaseRegistry();
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const JsonVariantConst folderValue = doc["opdsDownloadFolder"];
  if (!folderValue.isUnbound()) {
    std::string folder;
    const auto input = folderValue.as<JsonString>();
    if (!folderValue.is<const char*>() ||
        !OpdsDownloadFolder::normalize(std::string_view(input.c_str(), input.size()), folder)) {
      server->send(400, "text/plain", "Invalid download folder (maximum 63 bytes)");
      return;
    }
    doc["opdsDownloadFolder"] = folder == "/" ? "" : folder;
  }
  const std::string previousFolder = SETTINGS.opdsDownloadFolder;

  sdFontSystem.refreshIfDirty();
  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;
  uint8_t CrossPointSettings::* twoFingerSwipeEdited = nullptr;

  std::unique_lock<std::mutex> settingsLock(SETTINGS.getMutex());
  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = enumRawValueForDisplayIndex(s, static_cast<uint8_t>(val));
            QuickActions::settingChanged(SETTINGS, s.valuePtr);
            if (s.valuePtr == &CrossPointSettings::twoFingerSwipeUp ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeDown ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeLeft ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeRight) {
              twoFingerSwipeEdited = s.valuePtr;
            }
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.value16Ptr) {
            SETTINGS.*(s.value16Ptr) = static_cast<uint16_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (std::strcmp(s.key, "deviceName") == 0 && (val.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                                      val.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
          break;
        }
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  if (twoFingerSwipeEdited != nullptr) {
    CrossPointSettings::normalizeTwoFingerSwipeActions(SETTINGS, twoFingerSwipeEdited);
  }
  settingsLock.unlock();  // saveToFile acquires the settings mutex itself.
  if (!SETTINGS.saveToFile()) {
    if (!folderValue.isUnbound()) strcpy(SETTINGS.opdsDownloadFolder, previousFolder.c_str());
    LOG_ERR("WEB", "Failed to save settings");
    server->send(500, "text/plain", "Failed to save settings");
    sdFontSystem.releaseRegistry();
    return;
  }

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
  sdFontSystem.releaseRegistry();
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  // Reuse the bounded writer: longer folder paths and escaped URLs must not
  // truncate an object or produce a leading comma after a skipped server.
  WebJsonWriter json(*server);
  for (size_t i = 0; i < servers.size(); i++) {
    const auto& entry = servers[i];
    if (i > 0) json.append(",");
    json.append("{\"index\":");
    json.appendUnsigned(i);
    json.append(",\"name\":");
    json.appendJsonString(entry.name.c_str());
    json.append(",\"url\":");
    json.appendJsonString(entry.url.c_str());
    json.append(",\"username\":");
    json.appendJsonString(entry.username.c_str());
    json.append(",\"filenameFormat\":");
    json.appendJsonString(opdsFilenameFormatToJson(entry.filenameFormat));
    json.append(",\"downloadFolder\":");
    json.appendJsonString(entry.downloadFolder.c_str());
    json.append(entry.password.empty() ? ",\"hasPassword\":false}" : ",\"hasPassword\":true}");
    json.flush();
    yield();
  }
  json.append("]");
  json.flush();
  server->sendContent("");
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");
  opdsServer.filenameFormat = opdsFilenameFormatFromJson(doc["filenameFormat"] | "");

  if (!applyOpdsDownloadFolder(doc.as<JsonVariantConst>(), opdsServer.downloadFolder)) {
    server->send(400, "text/plain", "Invalid download folder (maximum 63 bytes)");
    return;
  }
  const bool hasDownloadFolder = !doc.as<JsonVariantConst>()["downloadFolder"].isUnbound();

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");
  const bool hasFilenameFormatField =
      doc["filenameFormat"].is<const char*>() || doc["filenameFormat"].is<std::string>();

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
    // Preserve existing values for fields older clients do not know how to send.
    if (existing && !hasPasswordField) {
      password = existing->password;
    }
    if (existing && !hasFilenameFormatField) {
      opdsServer.filenameFormat = existing->filenameFormat;
    }
    if (existing && !hasDownloadFolder) opdsServer.downloadFolder = existing->downloadFolder;
    opdsServer.password = password;
    if (!OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer)) {
      server->send(500, "text/plain", "Could not save OPDS server");
      return;
    }
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto credentials = WIFI_STORE.getCredentialSummaries();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = credentials[i].hasPassword;
    doc["isLastConnected"] = credentials[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credential->ssid;
    if (!hasPasswordField) {
      password = credential->password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
  if (!ssid) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  if (!WIFI_STORE.removeCredential(*ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid->c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str();
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Access denied to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          if (Storage.exists(filePath.c_str())) {
            LOG_DBG("WS", "Upload collision: %s", filePath.c_str());
            wsServer->sendTXT(num, "ERROR:File already exists: " + wsUploadFileName);
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Open file for writing
          sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            if (!clearBookCachePreservingUserState(filePath.c_str(), true)) {
              wsLastCompleteName = "";
              LOG_ERR("WS", "File saved but cache refresh failed: %s", filePath.c_str());
              wsServer->sendTXT(num, "ERROR:File saved, but could not refresh cached reading/sync data");
              return;
            }
            ImageFolderIndex::invalidateForPath(filePath.c_str());
            sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      size_t written = wsUploadFile.write(payload, length);

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache after uploading the file
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        if (!clearBookCachePreservingUserState(filePath.c_str(), true)) {
          wsLastCompleteName = "";
          LOG_ERR("WS", "File saved but cache refresh failed: %s", filePath.c_str());
          wsServer->sendTXT(num, "ERROR:File saved, but could not refresh cached reading/sync data");
          return;
        }
        ImageFolderIndex::invalidateForPath(filePath.c_str());
        sdFontSystem.markRegistryDirtyForPath(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendStaticContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml), FontsPageHtmlETag);
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  sdFontSystem.ensureRegistry();
  const auto& families = sdFontSystem.registry().getFamilies();

  // Send the catalog as it is enumerated. Building an ArduinoJson document and
  // then serializing it to a String keeps two complete copies in RAM, which
  // can exhaust the fragmented network heap with larger font collections.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  WebJsonWriter json(*server);
  json.append("{\"families\":[");

  bool firstFamily = true;
  for (const auto& family : families) {
    // Hydrate and emit one family's paths at a time. Keeping every family's
    // paths resident is what made larger catalogs exhaust the X3 network heap.
    if (!family.ensureDetails()) continue;

    if (!firstFamily) json.append(",");
    firstFamily = false;

    json.append("{\"name\":");
    json.appendJsonString(family.name.c_str());
    json.append(",\"sizes\":[");

    bool firstSize = true;
    for (uint8_t s : family.availableSizes()) {
      if (!firstSize) json.append(",");
      firstSize = false;
      json.appendUnsigned(s);
    }
    json.append("],\"files\":[");

    bool firstFile = true;
    family.ensureDetails();
    for (const auto& file : family.files) {
      if (!firstFile) json.append(",");
      firstFile = false;

      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      json.append("{\"name\":");
      json.appendJsonString(name ? name + 1 : file.path.c_str());

      // Stat the file for size
      HalFile f;
      unsigned long fileSize = 0;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileSize = static_cast<unsigned long>(f.size());
        f.close();
      }

      json.append(",\"size\":");
      json.appendUnsigned(fileSize);
      json.append("}");
      json.flush();
      yield();
    }
    json.append("]}");
    json.flush();
    family.releaseDetails();
    yield();
  }

#if CROSSINK_SCALABLE_FONTS
  json.append("],\"ttfSupported\":true,\"maxFamilies\":");
#else
  json.append("],\"ttfSupported\":false,\"maxFamilies\":");
#endif
  json.appendUnsigned(SdCardFontRegistry::MAX_SD_FAMILIES);
  json.append("}");
  json.flush();
  server->sendContent("");
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a supported font basename with safe path characters. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[192];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;
      sdFontSystem.markRegistryDirty();

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (dropUploadIfCancelled()) {
        abortFontUpload();
        break;
      }
      if (!fontUpload.valid) break;

      // Validate the complete file after closing it; multipart chunks may
      // split the signature at any byte boundary.
#if CROSSINK_SCALABLE_FONTS
      const auto& path = fontUpload.filePath;
      if (fontfiles::isScalable(path.c_str()) &&
          fontUpload.bytesWritten + fontUpload.bufferPos + upload.currentSize > 2 * 1024 * 1024) {
        fontUpload.valid = false;
        LOG_ERR("WEB", "Scalable font exceeds 2 MiB limit");
        break;
      }
#endif

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          if (fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos) != fontUpload.bufferPos) {
            abortFontUpload();
            break;
          }
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      if (dropUploadIfCancelled()) {
        abortFontUpload();
        break;
      }
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.valid =
            fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos) == fontUpload.bufferPos;
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.valid = fontUpload.file.sync() && fontUpload.valid;
        fontUpload.valid = fontUpload.file.close() && fontUpload.valid;
      }

      if (!fontUpload.filePath.empty())
        fontUpload.valid = FontInstaller::finishUpload(fontUpload.filePath.c_str(), fontUpload.valid);

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      abortFontUpload();
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Unsupported or damaged font file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  sdFontSystem.refreshIfDirty();
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}

void CrossPointWebServer::suspendTransferServices() {
  // Leave the WebSocket server alone mid-upload; killing it would abort the
  // transfer. The fetch just stalls that upload until it completes.
  if (wsServer && !wsUploadInProgress) {
    wsServer->close();
    wsServer.reset();
  }
  if (udpActive) udp.stop();
  LOG_DBG("WEB", "Transfer services suspended, heap %u, max block %u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}

void CrossPointWebServer::resumeTransferServices() {
  if (!running) return;
  if (!wsServer) {
    auto* ws = new (std::nothrow) WebSocketsServer(wsPort);
    if (ws) {
      wsServer.reset(ws);
      wsServer->begin();
      wsServer->onEvent(wsEventCallback);
    } else {
      LOG_ERR("WEB", "OOM: WebSocket server restart");
    }
  }
  if (udpActive) udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Transfer services resumed, heap %u, max block %u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}

void CrossPointWebServer::releasePluginRequest() {
  static_cast<CrossPointHttpServer*>(server.get())->releaseRequestArguments();
}
