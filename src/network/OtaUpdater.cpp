#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <cstring>

#include "AppVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef CROSSINK_OTA_RELEASE_URL
#define CROSSINK_OTA_RELEASE_URL "https://api.github.com/repos/uxjulia/CrossInk/releases/latest"
#endif

constexpr char latestReleaseUrl[] = CROSSINK_OTA_RELEASE_URL;

#ifdef CROSSPOINT_FIRMWARE_VARIANT
constexpr char firmwareAssetStem[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT;
constexpr char firmwareAssetName[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT ".bin";
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr int OTA_HTTP_READ_TIMEOUT_MS = 5000;
constexpr uint32_t OTA_DOWNLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr size_t OTA_READ_BUFFER_SIZE = 1024;
constexpr uint8_t OTA_MAX_REDIRECTS = 5;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  return parsed;
}

int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

bool isRedirectStatus(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->assign(evt->header_value);
  }
  return ESP_OK;
}

struct ParsedUrl {
  bool https = false;
  std::string host;
  std::string path;
  uint16_t port = 80;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;

  const std::string scheme = url.substr(0, schemeEnd);
  out.https = scheme == "https";
  if (!out.https && scheme != "http") return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  out.port = out.https ? 443 : 80;

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    const std::string portText = hostPort.substr(portSep + 1);
    if (portText.empty()) return false;
    uint32_t parsedPort = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return false;
      parsedPort = parsedPort * 10 + static_cast<uint32_t>(c - '0');
      if (parsedPort > UINT16_MAX) return false;
    }
    if (parsedPort == 0) return false;
    out.port = static_cast<uint16_t>(parsedPort);
  } else {
    out.host = hostPort;
  }

  return !out.host.empty() && !out.path.empty();
}

std::string buildRedirectUrl(const std::string& baseUrl, const std::string& location) {
  if (startsWith(location.c_str(), "http://") || startsWith(location.c_str(), "https://")) return location;

  ParsedUrl base;
  if (!parseUrl(baseUrl, base)) return location;

  std::string origin = base.https ? "https://" : "http://";
  origin += base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) {
    origin += ":";
    origin += std::to_string(base.port);
  }

  if (!location.empty() && location[0] == '/') return origin + location;

  const size_t lastSlash = base.path.rfind('/');
  const std::string parent = lastSlash == std::string::npos ? "/" : base.path.substr(0, lastSlash + 1);
  return origin + parent + location;
}

char lowerHex(const uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHexChar(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* value) {
  if (value == nullptr) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isHexChar(value[i])) return false;
  }
  return value[64] == '\0';
}

bool sha256Matches(const uint8_t digest[32], const char* expectedHex) {
  if (!isSha256Hex(expectedHex)) return false;

  for (size_t i = 0; i < 32; ++i) {
    const char high = lowerHex((digest[i] >> 4) & 0x0F);
    const char low = lowerHex(digest[i] & 0x0F);
    if (high != asciiLower(expectedHex[i * 2]) || low != asciiLower(expectedHex[i * 2 + 1])) return false;
  }
  return true;
}

void formatSha256(const uint8_t digest[32], char output[65]) {
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = lowerHex((digest[i] >> 4) & 0x0F);
    output[i * 2 + 1] = lowerHex(digest[i] & 0x0F);
  }
  output[64] = '\0';
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
}

size_t totalBytesReceived = 0;

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

esp_err_t release_manifest_event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

void notifyOtaProgress(OtaInstallContext* ctx, const bool force) {
  if (ctx == nullptr || ctx->onProgress == nullptr || ctx->processedSize == nullptr || ctx->totalSize == 0) return;

  const size_t processed = *ctx->processedSize;
  const int pct = static_cast<int>(static_cast<uint64_t>(processed) * 100 / ctx->totalSize);
  if (force || pct != ctx->lastReportedPct || processed - ctx->lastProgressBytes >= OTA_PROGRESS_UPDATE_BYTES) {
    ctx->lastReportedPct = pct;
    ctx->lastProgressBytes = processed;
    ctx->onProgress(ctx->progressCtx);
  }
}

void logTlsError(esp_http_client_handle_t client, const char* phase) {
  int tlsError = 0;
  int tlsFlags = 0;
  const esp_err_t err = esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
  if (err != ESP_OK || tlsError != 0 || tlsFlags != 0) {
    const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
    LOG_ERR("OTA", "%s TLS error: err=%s mbedtls=0x%x flags=0x%x", phase, esp_err_to_name(err), tlsCode, tlsFlags);
  }
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSha256.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  esp_err_t esp_err;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = release_manifest_event_handler,
      // 4096 holds the API response headers; the 32KB body streams through the
      // parser in chunks so RX needn't be larger. TX only carries our GET.
      // Both free before installUpdate, so smaller leaves it less fragmentation.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .user_data = &releaseParser,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSINK_VERSION);

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return HTTP_ERROR;
  }

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaSha256 = releaseParser.getFirmwareSha256();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
          otaSha256.empty() ? "missing" : "present");
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSINK_VERSION) {
    return false;
  }

  const int comparison = compareVersions(latestVersion.c_str(), CROSSINK_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), CROSSINK_VERSION,
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }
  if (isHttpUrl(otaUrl) && !isSha256Hex(otaSha256.c_str())) {
    LOG_ERR("OTA", "Refusing HTTP firmware URL without manifest sha256");
    return JSON_PARSE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %zu", otaSize, updatePartition->size);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  WifiPowerSaveGuard wifiPowerSaveGuard;

  LOG_INF("OTA", "Starting firmware download: url=%s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  auto buffer = makeUniqueNoThrow<char[]>(OTA_READ_BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("OTA", "Failed to allocate %zu byte OTA read buffer (heap=%u maxAlloc=%u)", OTA_READ_BUFFER_SIZE,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return OOM_ERROR;
  }

  std::string currentUrl = otaUrl;
  esp_http_client_handle_t client = nullptr;
  int64_t contentLength = -1;
  int statusCode = 0;
  esp_err_t esp_err = ESP_OK;

  for (uint8_t hop = 0; hop < OTA_MAX_REDIRECTS; ++hop) {
    std::string redirectLocation;
    esp_http_client_config_t client_config = {};
    client_config.url = currentUrl.c_str();
    client_config.timeout_ms = 15000;
    // 4096 holds the github->CDN redirect headers (the 512 default truncates
    // them); TX only carries our GET. Both are contiguous blocks contending
    // with the TLS handshake on a tight internal arena, so keep them minimal.
    client_config.buffer_size = 4096;
    client_config.buffer_size_tx = 1024;
    client_config.skip_cert_common_name_check = true;
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
    client_config.event_handler = captureLocationHeader;
    client_config.user_data = &redirectLocation;
    client_config.keep_alive_enable = false;
    client_config.disable_auto_redirect = true;

    client = esp_http_client_init(&client_config);
    if (client == nullptr) {
      LOG_ERR("OTA", "HTTP client init failed (heap=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return HTTP_ERROR;
    }

    esp_err = http_client_set_header_cb(client);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "Failed to set OTA User-Agent: %s", esp_err_to_name(esp_err));
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }

    LOG_INF("OTA", "Opening firmware connection");
    esp_err = esp_http_client_open(client, 0);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "Firmware HTTP open failed: %s (heap=%u maxAlloc=%u)", esp_err_to_name(esp_err), ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      logTlsError(client, "Firmware open failure");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    LOG_INF("OTA", "Fetching firmware headers");
    contentLength = esp_http_client_fetch_headers(client);
    statusCode = esp_http_client_get_status_code(client);
    if (contentLength < 0) {
      LOG_ERR("OTA", "Firmware header fetch failed: %lld", static_cast<long long>(contentLength));
      logTlsError(client, "Firmware header failure");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (!isRedirectStatus(statusCode)) {
      break;
    }

    if (redirectLocation.empty()) {
      LOG_ERR("OTA", "Firmware redirect missing Location header");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    const std::string redirectUrl = buildRedirectUrl(currentUrl, redirectLocation);
    ParsedUrl currentParsed;
    ParsedUrl redirectParsed;
    if (!parseUrl(redirectUrl, redirectParsed)) {
      LOG_ERR("OTA", "Rejected firmware redirect with unsupported Location");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (parseUrl(currentUrl, currentParsed) && currentParsed.https && !redirectParsed.https) {
      LOG_ERR("OTA", "Rejected firmware HTTPS downgrade redirect to %s", redirectParsed.host.c_str());
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    LOG_DBG("OTA", "Following firmware redirect to %s", redirectParsed.host.c_str());
    esp_http_client_cleanup(client);
    client = nullptr;
    currentUrl = redirectUrl;
  }

  if (client == nullptr) {
    LOG_ERR("OTA", "Firmware redirect limit exceeded");
    return HTTP_ERROR;
  }
  if (statusCode < 200 || statusCode >= 300) {
    LOG_ERR("OTA", "Firmware HTTP status: %d", statusCode);
    esp_http_client_cleanup(client);
    return HTTP_ERROR;
  }

  const size_t firmwareSize = contentLength > 0 ? static_cast<size_t>(contentLength) : otaSize;
  if (firmwareSize > 0) {
    if (firmwareSize > updatePartition->size) {
      LOG_ERR("OTA", "Firmware response too large: %zu > %zu", firmwareSize, updatePartition->size);
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }
    totalSize = firmwareSize;
    installCtx.totalSize = firmwareSize;
  }

  LOG_INF("OTA", "Writing firmware to %s @0x%x size=%zu heap=%u maxAlloc=%u", updatePartition->label,
          static_cast<unsigned>(updatePartition->address), firmwareSize, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  esp_err = esp_ota_begin(updatePartition, firmwareSize > 0 ? firmwareSize : OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s (heap=%u maxAlloc=%u)", esp_err_to_name(esp_err), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    esp_http_client_cleanup(client);
    return esp_err == ESP_ERR_NO_MEM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_timeout_ms(client, OTA_HTTP_READ_TIMEOUT_MS);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "Failed to set OTA read timeout: %s", esp_err_to_name(esp_err));
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return INTERNAL_UPDATE_ERROR;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);

  uint32_t lastReadMs = millis();
  while (true) {
    if (isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return CANCELLED_ERROR;
    }

    const int bytesRead = esp_http_client_read(client, buffer.get(), OTA_READ_BUFFER_SIZE);
    if (bytesRead < 0) {
      if (bytesRead == -ESP_ERR_HTTP_EAGAIN) {
        const uint32_t idleMs = millis() - lastReadMs;
        if (idleMs >= OTA_DOWNLOAD_IDLE_TIMEOUT_MS) {
          LOG_ERR("OTA", "Firmware read timed out after %zu/%zu bytes (idle=%lu ms)", processedSize, totalSize,
                  static_cast<unsigned long>(idleMs));
          mbedtls_sha256_free(&shaCtx);
          esp_ota_abort(otaHandle);
          esp_http_client_cleanup(client);
          return HTTP_ERROR;
        }
        delay(1);
        continue;
      }

      LOG_ERR("OTA", "Firmware read failed after %zu/%zu bytes", processedSize, totalSize);
      logTlsError(client, "Firmware read failure");
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (bytesRead == 0) break;

    esp_err = esp_ota_write(otaHandle, buffer.get(), static_cast<size_t>(bytesRead));
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_ota_write failed after %zu bytes: %s", processedSize, esp_err_to_name(esp_err));
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }

    mbedtls_sha256_update(&shaCtx, reinterpret_cast<const unsigned char*>(buffer.get()),
                          static_cast<size_t>(bytesRead));
    processedSize += static_cast<size_t>(bytesRead);
    lastReadMs = millis();
    notifyOtaProgress(&installCtx, false);
    if (totalSize > 0 && processedSize >= totalSize) break;
    delay(0);
  }

  if (isCancellationRequested()) {
    LOG_INF("OTA", "Update cancelled");
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return CANCELLED_ERROR;
  }

  if (!esp_http_client_is_complete_data_received(client)) {
    LOG_ERR("OTA", "Firmware download incomplete: %zu/%zu", processedSize, totalSize);
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return INTERNAL_UPDATE_ERROR;
  }
  esp_http_client_cleanup(client);

  notifyOtaProgress(&installCtx, true);

  uint8_t computedSha256[32];
  mbedtls_sha256_finish(&shaCtx, computedSha256);
  mbedtls_sha256_free(&shaCtx);
  if (!otaSha256.empty()) {
    if (!sha256Matches(computedSha256, otaSha256.c_str())) {
      char computedSha256Hex[65];
      formatSha256(computedSha256, computedSha256Hex);
      LOG_ERR("OTA", "Firmware sha256 mismatch: expected=%s actual=%s", otaSha256.c_str(), computedSha256Hex);
      esp_ota_abort(otaHandle);
      return HASH_MISMATCH_ERROR;
    }
    LOG_INF("OTA", "Firmware sha256 verified");
  }

  esp_err = esp_ota_end(otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed: %zu bytes", processedSize);
  return OK;
}
#endif
