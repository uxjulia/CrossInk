#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// App-facing transport for the CrossPoint stats snapshot API.
class StatsUploadClient {
 public:
  enum class Result { Ok, Network, Auth, Unsupported, InvalidResponse, LowMemory, Server, Skipped, Cancelled };
  // Foreground-only optional transport. USB supplies the computer's authenticated
  // HTTP response; all existing acknowledgment and merge validation still runs here.
  struct Relay {
    const char* server;
    const char* username;
    void* context;
    int (*request)(void*, const char* method, const char* endpoint, const char* payload, const char* inputPath,
                   char* response, size_t capacity, const char* outputPath);
    char response[256]{};
  };
  using Cancel = bool (*)(void*);
  static constexpr uint32_t FOREGROUND_TIMEOUT_MS = 15000;
  static constexpr uint32_t SLEEP_TIMEOUT_MS = 4000;
  explicit StatsUploadClient(Relay* relay = nullptr, Cancel cancel = nullptr, void* context = nullptr,
                             uint32_t timeoutMs = FOREGROUND_TIMEOUT_MS);

  ~StatsUploadClient();
  void close();
  // Zero for non-book requests; otherwise require acknowledgment of every book.
  Result put(const char* endpoint, const char* payload, unsigned bookCount, bool daily = false,
             const char* sessionId = nullptr);
  // Stream a staged JSON file when supported by the SDK; otherwise buffer in
  // PSRAM or internal RAM only when the normal TLS headroom can be retained.
  Result putFile(const char* endpoint, const char* path, unsigned bookCount);
  // Bounded extension download to a temporary SD file, closed before returning.
  // Releases TLS before parsing; the caller owns removal of responsePath.
  Result get(const char* endpoint, const char* responsePath, size_t maxBytes);
  // Read-only capability check against the stats summary endpoint. Records the
  // answer in the credential store; Unsupported means a KOSync-only server.
  // A non-JSON 200 (captive portal, proxy page) is only persisted when
  // `trustHtml` is set, i.e. right after the server accepted our credentials.
  Result probe(bool trustHtml = false);
  // Bounded calendar download. Old servers may omit this endpoint.
  Result downloadStreak();
  static int lastHttpCode;  // Foreground request; copy before starting another sync part.
  static const char* errorString(Result result);
  static const char* formatResult(char* buffer, size_t capacity, Result result, int httpCode);
  // A server without the extension API is not an upload failure: progress-only
  // servers simply have nothing to receive stats or clippings.
  static bool failed(Result result) {
    return result != Result::Ok && result != Result::Skipped && result != Result::Unsupported;
  }
  static const char* resultString(Result result);
  // The upload is done with: sent, or nothing to send. "Unsupported" is not:
  // the data still has to go once the server accepts it.
  static bool settled(Result result) { return result == Result::Ok || result == Result::Skipped; }
  static bool deviceId(char* out, size_t capacity);

 private:
  Result putBody(const char* endpoint, const char* payload, unsigned bookCount, bool daily, const char* sessionId,
                 const char* filePath);
  Result acceptUpload(const char* endpoint, unsigned bookCount, bool daily, const char* sessionId, int status,
                      bool complete, const char* response);
  Result acceptStreak(int status, bool complete, const char* response);
  Relay* relay;
  Cancel cancel;
  void* cancelContext;
  uint32_t timeoutMs;
  bool cancelled() const { return cancel && cancel(cancelContext); }
  struct Impl;
  std::unique_ptr<Impl> impl;
};
