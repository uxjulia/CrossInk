#pragma once

#include <memory>

// App-facing transport for the CrossPoint stats snapshot API.
class StatsUploadClient {
 public:
  enum class Result { Ok, Network, Auth, Unsupported, InvalidResponse, LowMemory, Server, Skipped, Cancelled };
  StatsUploadClient();
  ~StatsUploadClient();
  void close();
  // Zero for non-book requests; otherwise require acknowledgment of every book.
  Result put(const char* endpoint, const char* payload, unsigned bookCount, bool daily = false,
             const char* sessionId = nullptr);
  // Upload a staged JSON file through a fixed transport window; no full payload in RAM.
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
  static const char* errorString(Result result);
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
  struct Impl;
  std::unique_ptr<Impl> impl;
};
