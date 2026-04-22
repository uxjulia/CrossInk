#ifdef SIMULATOR
#include "OtaUpdater.h"
bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(volatile bool*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>

#include <cstdio>
#include <memory>

#include "OtaUpdater.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/uxjulia/crossink-reader/releases/latest";
constexpr int otaHttpBufferSize = 4096;
constexpr int otaHttpRequestSize = 4096;
constexpr int otaMaxRedirects = 5;
constexpr size_t otaImageHeaderBufferSize =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

#ifdef CROSSPOINT_FIRMWARE_VARIANT
constexpr char firmwareAssetName[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT ".bin";
#else
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

/* This is buffer and size holder to keep upcoming data from latestReleaseUrl */
char* local_buf;
int output_len;

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

esp_err_t event_handler(esp_http_client_event_t* event) {
  /* We do interested in only HTTP_EVENT_ON_DATA event only */
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

  if (!esp_http_client_is_chunked_response(event->client)) {
    int content_len = esp_http_client_get_content_length(event->client);
    int copy_len = 0;

    if (local_buf == NULL) {
      /* local_buf life span is tracked by caller checkForUpdate */
      local_buf = static_cast<char*>(calloc(content_len + 1, sizeof(char)));
      output_len = 0;
      if (local_buf == NULL) {
        LOG_ERR("OTA", "HTTP Client Out of Memory Failed, Allocation %d", content_len);
        return ESP_ERR_NO_MEM;
      }
    }
    copy_len = min(event->data_len, (content_len - output_len));
    if (copy_len) {
      memcpy(local_buf + output_len, event->data, copy_len);
    }
    output_len += copy_len;
  } else {
    /* Code might be hits here, It happened once (for version checking) but I need more logs to handle that */
    int chunked_len;
    esp_http_client_get_chunk_length(event->client, &chunked_len);
    LOG_DBG("OTA", "esp_http_client_is_chunked_response failed, chunked_len: %d", chunked_len);
  }

  return ESP_OK;
} /* event_handler */

void logPartitionInfo(const char* label, const esp_partition_t* partition) {
  if (!partition) {
    LOG_ERR("OTA", "%s partition not found", label);
    return;
  }

  LOG_INF("OTA", "%s partition: label=%s subtype=%d addr=0x%lx size=%lu", label, partition->label, partition->subtype,
          static_cast<unsigned long>(partition->address), static_cast<unsigned long>(partition->size));
}

void logAppDescription(const char* label, const esp_app_desc_t& appDesc) {
  LOG_INF("OTA", "%s image: project=%s version=%s secure_ver=%lu min_rev=%u max_rev=%u", label, appDesc.project_name,
          appDesc.version, static_cast<unsigned long>(appDesc.secure_version), appDesc.min_efuse_blk_rev_full,
          appDesc.max_efuse_blk_rev_full);
}

void logRawImageHeader(const char* label, const esp_partition_t* partition) {
  if (!partition) {
    LOG_ERR("OTA", "%s raw header skipped: partition missing", label);
    return;
  }

  esp_image_header_t header = {};
  const esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "%s raw header read failed: %s", label, esp_err_to_name(err));
    return;
  }

  LOG_INF(
      "OTA",
      "%s raw header: magic=0x%02x segments=%u chip_id=%u min_rev=%u min_rev_full=%u max_rev_full=%u hash_appended=%u",
      label, header.magic, header.segment_count, static_cast<unsigned>(header.chip_id),
      static_cast<unsigned>(header.min_chip_rev), static_cast<unsigned>(header.min_chip_rev_full),
      static_cast<unsigned>(header.max_chip_rev_full), static_cast<unsigned>(header.hash_appended));
}

void logPartitionSha256(const char* label, const esp_partition_t* partition) {
  if (!partition) {
    LOG_ERR("OTA", "%s sha256 skipped: partition missing", label);
    return;
  }

  uint8_t sha[32] = {};
  const esp_err_t err = esp_partition_get_sha256(partition, sha);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "%s sha256 read failed: %s", label, esp_err_to_name(err));
    return;
  }

  char shaHex[65] = {};
  for (int i = 0; i < 32; ++i) {
    snprintf(&shaHex[i * 2], 3, "%02x", sha[i]);
  }
  LOG_INF("OTA", "%s sha256: %s", label, shaHex);
}

void logRuntimeHeadroom(const char* label) {
  LOG_INF("OTA", "%s runtime: free_heap=%u min_free_heap=%u max_alloc=%u stack_hwm=%u", label, ESP.getFreeHeap(),
          ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

bool isRedirectStatus(const int statusCode) {
  return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308;
}
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  JsonDocument filter;
  esp_err_t esp_err;
  JsonDocument doc;

  esp_http_client_config_t client_config = {
      .url = latestReleaseUrl,
      .event_handler = event_handler,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  /* To track life time of local_buf, dtor will be called on exit from that function */
  struct localBufCleaner {
    char** bufPtr;
    ~localBufCleaner() {
      if (*bufPtr) {
        free(*bufPtr);
        *bufPtr = NULL;
      }
    }
  } localBufCleaner = {&local_buf};

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

  /* esp_http_client_close will be called inside cleanup as well*/
  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  const DeserializationError error = deserializeJson(doc, local_buf, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();

  LOG_DBG("OTA", "Looking for asset: %s", firmwareAssetName);
  for (int i = 0; i < doc["assets"].size(); i++) {
    if (doc["assets"][i]["name"] == firmwareAssetName) {
      otaUrl = doc["assets"][i]["browser_download_url"].as<std::string>();
      otaSize = doc["assets"][i]["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No %s asset found in release", firmwareAssetName);
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found update: %s", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  const char* latestVersionNormalized = latestVersion.c_str();
  if (*latestVersionNormalized == 'v') latestVersionNormalized++;
  if (!updateAvailable || latestVersion.empty() || strcmp(latestVersionNormalized, CROSSINK_VERSION) == 0) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSINK_VERSION;

  // semantic version check (only match on 3 segments)
  // GitHub tags use a "v" prefix (e.g. "v1.2.5"); skip it so %d can parse the number.
  const char* latestRaw = latestVersion.c_str();
  if (*latestRaw == 'v') latestRaw++;
  sscanf(latestRaw, "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(volatile bool* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && *cancelRequested;
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  logPartitionInfo("Running", runningPartition);
  logPartitionInfo("Next OTA", updatePartition);

  esp_app_desc_t runningAppInfo = {};
  esp_err_t runningInfoErr = esp_ota_get_partition_description(runningPartition, &runningAppInfo);
  if (runningInfoErr == ESP_OK) {
    logAppDescription("Running", runningAppInfo);
  } else {
    LOG_ERR("OTA", "esp_ota_get_partition_description(running) Failed: %s", esp_err_to_name(runningInfoErr));
  }
  logRawImageHeader("Running", runningPartition);
  logPartitionSha256("Running", runningPartition);

  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available for update");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware image too large for OTA partition: image=%lu partition=%lu",
            static_cast<unsigned long>(otaSize), static_cast<unsigned long>(updatePartition->size));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_http_client_handle_t client_handle = nullptr;
  esp_ota_handle_t otaHandle = 0;
  bool otaBegun = false;
  OtaUpdaterError installError = OK;
  esp_err_t esp_err;
  /* Signal for OtaUpdateActivity */
  render.store(false);
  finalizing.store(false);
  processedSize = 0;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      .max_redirection_count = otaMaxRedirects,
      /* Default HTTP client buffer size 512 byte only
       * not sufficient to handle URL redirection cases or
       * parsing of large HTTP headers.
       */
      .buffer_size = otaHttpBufferSize,
      .buffer_size_tx = otaHttpBufferSize,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }

  if (isCancellationRequested()) {
    LOG_INF("OTA", "OTA install cancelled before download started");
    esp_http_client_cleanup(client_handle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return CANCELLED_ERROR;
  }

  esp_err = http_client_set_header_cb(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed: %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }

  int contentLength = -1;
  int statusCode = 0;
  bool headersReady = false;
  for (int redirectCount = 0; redirectCount <= otaMaxRedirects; ++redirectCount) {
    if (isCancellationRequested()) {
      LOG_INF("OTA", "OTA install cancelled before opening HTTP stream");
      esp_http_client_cleanup(client_handle);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      return CANCELLED_ERROR;
    }

    esp_err = esp_http_client_open(client_handle, 0);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_http_client_open Failed: %s", esp_err_to_name(esp_err));
      esp_http_client_cleanup(client_handle);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      return HTTP_ERROR;
    }

    contentLength = esp_http_client_fetch_headers(client_handle);
    statusCode = esp_http_client_get_status_code(client_handle);

    if (contentLength < 0 && !isRedirectStatus(statusCode)) {
      LOG_ERR("OTA", "esp_http_client_fetch_headers Failed: %d", contentLength);
      esp_http_client_close(client_handle);
      esp_http_client_cleanup(client_handle);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      return HTTP_ERROR;
    }

    if (isRedirectStatus(statusCode)) {
      LOG_INF("OTA", "Following OTA redirect: status=%d", statusCode);
      esp_err = esp_http_client_set_redirection(client_handle);
      esp_http_client_close(client_handle);
      if (esp_err != ESP_OK) {
        LOG_ERR("OTA", "esp_http_client_set_redirection Failed: %s", esp_err_to_name(esp_err));
        esp_http_client_cleanup(client_handle);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return HTTP_ERROR;
      }
      continue;
    }

    if (statusCode < 200 || statusCode >= 300) {
      LOG_ERR("OTA", "Unexpected OTA HTTP status: %d", statusCode);
      esp_http_client_close(client_handle);
      esp_http_client_cleanup(client_handle);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      return HTTP_ERROR;
    }

    headersReady = true;
    break;
  }

  if (!headersReady) {
    LOG_ERR("OTA", "Too many OTA redirects");
    esp_http_client_cleanup(client_handle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return HTTP_ERROR;
  }

  if (contentLength > 0) {
    totalSize = static_cast<size_t>(contentLength);
  }

  if (isCancellationRequested()) {
    LOG_INF("OTA", "OTA install cancelled before esp_ota_begin");
    esp_http_client_close(client_handle);
    esp_http_client_cleanup(client_handle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return CANCELLED_ERROR;
  }

  esp_err = esp_ota_begin(updatePartition, totalSize > 0 ? totalSize : OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin Failed: %s", esp_err_to_name(esp_err));
    esp_http_client_close(client_handle);
    esp_http_client_cleanup(client_handle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }
  otaBegun = true;

  std::unique_ptr<uint8_t[]> otaBuffer(new (std::nothrow) uint8_t[otaHttpRequestSize]);
  std::unique_ptr<uint8_t[]> imageHeaderBuffer(new (std::nothrow) uint8_t[otaImageHeaderBufferSize]());
  if (!otaBuffer || !imageHeaderBuffer) {
    LOG_ERR("OTA", "Failed to allocate OTA buffers");
    esp_ota_abort(otaHandle);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_http_client_close(client_handle);
    esp_http_client_cleanup(client_handle);
    return OOM_ERROR;
  }
  size_t imageHeaderBytes = 0;
  unsigned long lastRenderMs = 0;
  bool incomingImageLogged = false;
  while (true) {
    if (isCancellationRequested()) {
      esp_err = ESP_ERR_INVALID_STATE;
      installError = CANCELLED_ERROR;
      LOG_INF("OTA", "OTA install cancelled during download");
      break;
    }

    const int bytesRead =
        esp_http_client_read(client_handle, reinterpret_cast<char*>(otaBuffer.get()), otaHttpRequestSize);
    if (bytesRead < 0) {
      esp_err = ESP_FAIL;
      installError = HTTP_ERROR;
      LOG_ERR("OTA", "esp_http_client_read Failed: %d", bytesRead);
      break;
    }
    if (bytesRead == 0) {
      if (esp_http_client_is_complete_data_received(client_handle)) {
        esp_err = ESP_OK;
      } else {
        esp_err = ESP_FAIL;
        installError = HTTP_ERROR;
        LOG_ERR("OTA", "OTA download ended before complete data was received");
      }
      break;
    }

    if (!incomingImageLogged && imageHeaderBytes < otaImageHeaderBufferSize) {
      const size_t headerCopyLen = min(otaImageHeaderBufferSize - imageHeaderBytes, static_cast<size_t>(bytesRead));
      memcpy(imageHeaderBuffer.get() + imageHeaderBytes, otaBuffer.get(), headerCopyLen);
      imageHeaderBytes += headerCopyLen;
      if (imageHeaderBytes == otaImageHeaderBufferSize) {
        const auto* appDesc = reinterpret_cast<const esp_app_desc_t*>(
            imageHeaderBuffer.get() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
        logAppDescription("Incoming", *appDesc);
        incomingImageLogged = true;
      }
    }

    esp_err = esp_ota_write(otaHandle, otaBuffer.get(), static_cast<size_t>(bytesRead));
    if (esp_err != ESP_OK) {
      installError = INTERNAL_UPDATE_ERROR;
      LOG_ERR("OTA", "esp_ota_write Failed at %lu: %s", static_cast<unsigned long>(processedSize),
              esp_err_to_name(esp_err));
      break;
    }

    processedSize += static_cast<size_t>(bytesRead);
    if (isCancellationRequested()) {
      esp_err = ESP_ERR_INVALID_STATE;
      installError = CANCELLED_ERROR;
      LOG_INF("OTA", "OTA install cancelled after writing %lu bytes", static_cast<unsigned long>(processedSize));
      break;
    }

    /* Throttle UI refresh requests without slowing down the OTA loop itself. */
    const unsigned long now = millis();
    const bool nearingFinalize = totalSize > 0 && processedSize >= totalSize;
    if (!nearingFinalize && now - lastRenderMs >= 100) {
      render.store(true);
      lastRenderMs = now;
    }
  }

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  esp_http_client_close(client_handle);
  esp_http_client_cleanup(client_handle);

  if (esp_err != ESP_OK) {
    if (installError == CANCELLED_ERROR) {
      LOG_INF("OTA", "OTA install cancelled cleanly after reading %lu bytes",
              static_cast<unsigned long>(processedSize));
    } else {
      LOG_ERR("OTA", "OTA download/write Failed: %s, read=%lu, expected=%lu", esp_err_to_name(esp_err),
              static_cast<unsigned long>(processedSize), static_cast<unsigned long>(totalSize));
    }
    if (otaBegun) {
      esp_ota_abort(otaHandle);
    }
    finalizing.store(false);
    render.store(false);
    return installError;
  }

  if (totalSize > 0 && processedSize != totalSize) {
    LOG_ERR("OTA", "OTA size mismatch: read=%lu, expected=%lu", static_cast<unsigned long>(processedSize),
            static_cast<unsigned long>(totalSize));
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  finalizing.store(true);
  render.store(false);
  logRuntimeHeadroom("Before esp_ota_end");

  if (isCancellationRequested()) {
    LOG_INF("OTA", "OTA install cancelled before finalizing");
    finalizing.store(false);
    esp_ota_abort(otaHandle);
    return CANCELLED_ERROR;
  }

  esp_app_desc_t stagedAppInfo = {};
  esp_err = esp_ota_get_partition_description(updatePartition, &stagedAppInfo);
  if (esp_err == ESP_OK) {
    logAppDescription("Staged", stagedAppInfo);
  } else {
    LOG_ERR("OTA", "esp_ota_get_partition_description Failed: %s", esp_err_to_name(esp_err));
  }
  logRawImageHeader("Staged", updatePartition);
  logPartitionSha256("Staged", updatePartition);

  if (isCancellationRequested()) {
    LOG_INF("OTA", "OTA install cancelled before esp_ota_end");
    finalizing.store(false);
    esp_ota_abort(otaHandle);
    return CANCELLED_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);
  otaBegun = false;
  if (esp_err != ESP_OK) {
    logRuntimeHeadroom("esp_ota_end failed");
    LOG_ERR("OTA", "esp_ota_end Failed: %s, read=%lu, expected=%lu", esp_err_to_name(esp_err),
            static_cast<unsigned long>(processedSize), static_cast<unsigned long>(totalSize));
    finalizing.store(false);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    logRuntimeHeadroom("set_boot failed");
    LOG_ERR("OTA", "esp_ota_set_boot_partition Failed: %s, read=%lu, expected=%lu", esp_err_to_name(esp_err),
            static_cast<unsigned long>(processedSize), static_cast<unsigned long>(totalSize));
    finalizing.store(false);
    return INTERNAL_UPDATE_ERROR;
  }

  logRuntimeHeadroom("After esp_ota_end");
  finalizing.store(false);
  LOG_INF("OTA", "Update completed");
  return OK;
}
#endif
