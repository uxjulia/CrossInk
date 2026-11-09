#include "SyncLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

namespace {
// Allowlist routes instead of logging arbitrary URLs, query values or hashes.
const char* safeRoute(const char* endpoint) {
  for (const char* route :
       {"/api/v1/stats/counters/global", "/api/v1/stats/counters/books", "/api/v1/stats/global", "/api/v1/stats/books",
        "/api/v1/stats/summary", "/api/v1/stats/sessions", "/api/v1/stats/streak", "/api/v1/progress/changes",
        "/api/v1/progress/batch", "/users/auth", "/users/create"}) {
    const size_t length = strlen(route);
    if (strncmp(endpoint, route, length) == 0 && (endpoint[length] == '\0' || endpoint[length] == '?')) return route;
  }
  if (strncmp(endpoint, "/api/v1/clippings/", 18) == 0) return "/api/v1/clippings/:document";
  if (strcmp(endpoint, "/api/v1/clippings") == 0) return "/api/v1/clippings";
  if (strncmp(endpoint, "/syncs/progress/", 16) == 0) return "/syncs/progress/:document";
  if (strcmp(endpoint, "/syncs/progress") == 0) return "/syncs/progress";
  return "sync";
}
}  // namespace

bool SyncLog::record(const char* method, const char* endpoint, const int httpCode, const char* outcome) {
  // Fixed, bounded scratch; no retained log buffer or task while reading.
  char line[224];
  const auto now = time(nullptr);
  const int length =
      snprintf(line, sizeof(line), "utc=%lld uptime_ms=%lu %s %s http=%d result=%s\n", static_cast<long long>(now),
               static_cast<unsigned long>(millis()), method, safeRoute(endpoint), httpCode, outcome);
  if (length < 0 || size_t(length) >= sizeof(line)) {
    LOG_ERR("SyncLog", "Log entry too long");
    return false;
  }
  if (!Storage.ensureDirectoryExists("/.crossink")) {
    LOG_ERR("SyncLog", "Cannot create log folder");
    return false;
  }
  auto file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("SyncLog", "Cannot open sync log");
    file.close();
    return false;
  }
  if (file.size() + size_t(length) > MAX_BYTES) {
    if (!file.close() || (Storage.exists(PREVIOUS_PATH) && !Storage.remove(PREVIOUS_PATH)) ||
        !Storage.rename(PATH, PREVIOUS_PATH)) {
      LOG_ERR("SyncLog", "Cannot rotate sync log");
      return false;
    }
    file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (!file) {
      LOG_ERR("SyncLog", "Cannot open rotated sync log");
      file.close();
      return false;
    }
  }
  const bool written = file.write(line, size_t(length)) == size_t(length) && file.sync();
  const bool closed = file.close();
  if (!written || !closed) LOG_ERR("SyncLog", "Cannot save complete sync log entry");
  return written && closed;
}
