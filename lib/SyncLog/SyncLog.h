#pragma once

#include <cstddef>

// Foreground sync diagnostics only. Never pass server URLs, headers, payloads,
// book names or server-provided messages to this log.
namespace SyncLog {
inline constexpr const char* PATH = "/.crossink/sync.log";
inline constexpr const char* PREVIOUS_PATH = "/.crossink/sync.previous.log";
inline constexpr size_t MAX_BYTES = 64 * 1024;
bool record(const char* method, const char* endpoint, int httpCode, const char* outcome);

// Declared before request resources so the log is written after their cleanup.
class Request {
 public:
  Request(const char* method, const char* endpoint, int& httpCode)
      : method(method), endpoint(endpoint), httpCode(httpCode) {
    httpCode = 0;
  }
  ~Request() { record(method, endpoint, httpCode, outcome); }
  void finish(const char* value) { outcome = value; }

 private:
  const char* method;
  const char* endpoint;
  int& httpCode;
  const char* outcome = "incomplete";
};
}  // namespace SyncLog
