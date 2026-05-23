#include "HttpDownloader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <WiFi.h>
#include <base64.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "AppVersion.h"
#include "network/WifiPowerSaveGuard.h"
#include "util/UrlUtils.h"

namespace {
constexpr size_t PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr uint32_t PROGRESS_UPDATE_MS = 250;
constexpr size_t DEFAULT_DOWNLOAD_BUFFER_SIZE = 1024;
constexpr uint16_t HTTP_RESPONSE_TIMEOUT_MS = 15000;
constexpr int32_t HTTP_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t HTTPS_HANDSHAKE_TIMEOUT_SECONDS = 10;
constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 30000;

void logNetworkState(const char* phase) {
  LOG_DBG("HTTP", "%s: heap free=%u maxAlloc=%u wifi=%d rssi=%d", phase, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<int>(WiFi.status()), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

void logDownloadState(const char* phase, const size_t downloaded, const size_t total, const uint32_t idleMs) {
  LOG_ERR("HTTP", "%s after %zu/%zu bytes (idle=%lu ms, timeout=%lu ms)", phase, downloaded, total,
          static_cast<unsigned long>(idleMs), static_cast<unsigned long>(DOWNLOAD_IDLE_TIMEOUT_MS));
  logNetworkState(phase);
}

bool isCancelRequested(bool* cancelFlag, const HttpDownloader::CancelCallback& shouldCancel) {
  if (cancelFlag && *cancelFlag) {
    return true;
  }
  if (shouldCancel && shouldCancel()) {
    if (cancelFlag) {
      *cancelFlag = true;
    }
    return true;
  }
  return false;
}

class ProgressNotifier {
 public:
  ProgressNotifier(size_t total, HttpDownloader::ProgressCallback progress)
      : total_(total), progress_(std::move(progress)) {}

  void notify(size_t downloaded, bool force) {
    if (progress_ && total_ > 0) {
      const uint32_t now = millis();
      if (force || downloaded == total_ || downloaded - lastProgressBytes_ >= PROGRESS_UPDATE_BYTES ||
          now - lastProgressMs_ >= PROGRESS_UPDATE_MS) {
        lastProgressBytes_ = downloaded;
        lastProgressMs_ = now;
        progress_(downloaded, total_);
      }
    }
  }

 private:
  size_t total_;
  size_t lastProgressBytes_ = 0;
  uint32_t lastProgressMs_ = 0;
  HttpDownloader::ProgressCallback progress_;
};

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress, bool* cancelFlag,
                  HttpDownloader::CancelCallback shouldCancel)
      : file_(file),
        progress_(total, std::move(progress)),
        cancelFlag_(cancelFlag),
        shouldCancel_(std::move(shouldCancel)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!writeOk_) {
      return 0;
    }

    if (isCancelRequested(cancelFlag_, shouldCancel_)) {
      writeOk_ = false;
      return 0;
    }
    const size_t accepted = file_.write(buffer, size);
    if (accepted != size) {
      writeOk_ = false;
    }
    downloaded_ += accepted;
    progress_.notify(downloaded_, false);
    return accepted;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }
  void finishProgress() { progress_.notify(downloaded_, true); }

 private:
  FsFile& file_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  ProgressNotifier progress_;
  bool* cancelFlag_;
  HttpDownloader::CancelCallback shouldCancel_;
};

HttpDownloader::DownloadError downloadKnownLengthBody(HTTPClient& http, FsFile& file, const size_t contentLength,
                                                      HttpDownloader::ProgressCallback progress, size_t& downloaded,
                                                      bool* cancelFlag, const size_t bufferSize,
                                                      const HttpDownloader::CancelCallback& shouldCancel) {
  auto* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERR("HTTP", "Failed to get response stream");
    return HttpDownloader::HTTP_ERROR;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[bufferSize]);
  if (!buffer) {
    LOG_ERR("HTTP", "Failed to allocate %zu byte download buffer", bufferSize);
    return HttpDownloader::HTTP_ERROR;
  }

  ProgressNotifier progressNotifier(contentLength, std::move(progress));
  uint32_t lastProgressMs = millis();
  while (downloaded < contentLength) {
    if (isCancelRequested(cancelFlag, shouldCancel)) {
      return HttpDownloader::ABORTED;
    }
    const size_t remaining = contentLength - downloaded;

    int available = stream->available();
    if (available <= 0) {
      if (!http.connected()) {
        logDownloadState("Connection closed", downloaded, contentLength, millis() - lastProgressMs);
        return HttpDownloader::HTTP_ERROR;
      }
      if (millis() - lastProgressMs >= DOWNLOAD_IDLE_TIMEOUT_MS) {
        logDownloadState("Read timed out", downloaded, contentLength, millis() - lastProgressMs);
        return HttpDownloader::HTTP_ERROR;
      }
      delay(1);
      continue;
    }

    const size_t toRead = std::min({bufferSize, remaining, static_cast<size_t>(available)});
    const size_t bytesRead = stream->readBytes(buffer.get(), toRead);
    if (bytesRead == 0) {
      if (millis() - lastProgressMs < DOWNLOAD_IDLE_TIMEOUT_MS) {
        delay(1);
        continue;
      }
      logDownloadState("Read timed out", downloaded, contentLength, millis() - lastProgressMs);
      return HttpDownloader::HTTP_ERROR;
    }

    const size_t accepted = file.write(buffer.get(), bytesRead);
    downloaded += accepted;
    lastProgressMs = millis();
    progressNotifier.notify(downloaded, false);

    if (accepted != bytesRead) {
      logDownloadState("Write failed", downloaded, contentLength, 0);
      return HttpDownloader::FILE_ERROR;
    }

    delay(0);
  }

  progressNotifier.notify(downloaded, true);
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "Failed to allocate secure client");
      return false;
    }
    secureClient->setHandshakeTimeout(HTTPS_HANDSHAKE_TIMEOUT_SECONDS);
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "Failed to allocate client");
      return false;
    }
    client.reset(plainClient);
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  const int writeResult = http.writeToStream(&outContent);

  http.end();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream error: %d (%s)", writeResult, HTTPClient::errorToString(writeResult).c_str());
    return false;
  }

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  StreamString stream;
  if (!fetchUrl(url, stream, username, password)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new (std::nothrow) NetworkClientSecure();
    if (!secureClient) {
      LOG_ERR("HTTP", "Failed to allocate secure client");
      return HTTP_ERROR;
    }
    secureClient->setHandshakeTimeout(HTTPS_HANDSHAKE_TIMEOUT_SECONDS);
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    auto* plainClient = new (std::nothrow) NetworkClient();
    if (!plainClient) {
      LOG_ERR("HTTP", "Failed to allocate client");
      return HTTP_ERROR;
    }
    client.reset(plainClient);
  }
  HTTPClient http;
  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());
  LOG_DBG("HTTP", "Timeouts: connect=%ld ms response=%u ms idle=%lu ms buffer=%zu bytes",
          static_cast<long>(HTTP_CONNECT_TIMEOUT_MS), HTTP_RESPONSE_TIMEOUT_MS,
          static_cast<unsigned long>(DOWNLOAD_IDLE_TIMEOUT_MS), bufferSize);

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);

  size_t resumeOffset = 0;
  if (options.resumePartial && Storage.exists(destPath.c_str())) {
    FsFile existingFile;
    if (Storage.openFileForRead("HTTP", destPath.c_str(), existingFile)) {
      resumeOffset = existingFile.fileSize();
      existingFile.close();
    }
  }
  if (resumeOffset > 0) {
    char rangeHeader[40];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", resumeOffset);
    http.addHeader("Range", rangeHeader);
    LOG_DBG("HTTP", "Resuming download at byte %zu", resumeOffset);
  }

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  const bool isResumeResponse = resumeOffset > 0 && httpCode == 206;
  if (httpCode != HTTP_CODE_OK && !isResumeResponse) {
    if (httpCode < 0) {
      LOG_ERR("HTTP", "Download failed: %d (%s)", httpCode, HTTPClient::errorToString(httpCode).c_str());
      logNetworkState("Download failure");
    } else {
      LOG_ERR("HTTP", "Download failed: %d", httpCode);
    }
    http.end();
    return HTTP_ERROR;
  }
  if (resumeOffset > 0 && !isResumeResponse) {
    LOG_DBG("HTTP", "Server ignored range request; restarting download");
    Storage.remove(destPath.c_str());
    resumeOffset = 0;
  }

  const int64_t reportedLength = http.getSize();
  const size_t responseLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  const size_t contentLength = responseLength > 0 ? resumeOffset + responseLength : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }
  if (resumeOffset > 0) {
    LOG_DBG("HTTP", "Resume offset: %zu bytes", resumeOffset);
  }

  // Remove existing file if present, unless this is a resumable append.
  if (resumeOffset == 0 && Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (resumeOffset > 0) {
    file = Storage.open(destPath.c_str(), O_WRONLY | O_APPEND);
  } else if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }
  if (!file) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  size_t downloaded = resumeOffset;
  DownloadError transferError = OK;
  int writeResult = 0;

  if (contentLength > 0) {
    transferError = downloadKnownLengthBody(http, file, contentLength, std::move(progress), downloaded, cancelFlag,
                                            bufferSize, options.shouldCancel);
  } else {
    // Let HTTPClient handle chunked decoding and stream body bytes into the file.
    FileWriteStream fileStream(file, contentLength, std::move(progress), cancelFlag, std::move(options.shouldCancel));
    writeResult = http.writeToStream(&fileStream);
    fileStream.finishProgress();
    downloaded = fileStream.downloaded();
    if (cancelFlag && *cancelFlag) {
      transferError = ABORTED;
    } else if (writeResult < 0) {
      transferError = HTTP_ERROR;
    } else if (!fileStream.ok()) {
      LOG_ERR("HTTP", "Write failed during download");
      transferError = FILE_ERROR;
    }
  }

  file.flush();

  file.close();
  http.end();

  if (transferError != OK) {
    if (writeResult < 0) {
      LOG_ERR("HTTP", "writeToStream error: %d (%s)", writeResult, HTTPClient::errorToString(writeResult).c_str());
    }
    LOG_ERR("HTTP", "Transfer failed: error=%d downloaded=%zu expected=%zu preservePartial=%d resumePartial=%d",
            static_cast<int>(transferError), downloaded, contentLength, options.preservePartial, options.resumePartial);
    if (transferError == ABORTED || !options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return transferError;
  }

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    if (!options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    if (!options.preservePartial) {
      Storage.remove(destPath.c_str());
    }
    return HTTP_ERROR;
  }

  return OK;
}
