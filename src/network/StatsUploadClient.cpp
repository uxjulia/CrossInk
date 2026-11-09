#include "StatsUploadClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <SyncLog.h>
#include <mbedtls/base64.h>
#ifndef SIMULATOR
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "activities/reader/GlobalReadingStats.h"

int StatsUploadClient::lastHttpCode = 0;

namespace {
bool insufficientTlsHeap() {
  return ESP.getFreeHeap() < KOReaderSyncClient::MIN_FREE_HEAP_FOR_TLS ||
         ESP.getMaxAllocHeap() < KOReaderSyncClient::MIN_MAX_ALLOC_HEAP_FOR_TLS;
}

// Only definitive answers update the persisted capability; transient failures keep it.
void recordMissingApi(const int status) {
  if (status == 404 || status == 405) KOREADER_STORE.setServerSupport(SyncServerSupport::UNSUPPORTED);
}
}  // namespace

struct StatsUploadClient::Impl {
  freeink::SecureHttpClient http;
  char response[256]{};
};

// Keep the client and bounded reply buffer off the small main-task stack.
StatsUploadClient::StatsUploadClient() : impl(makeUniqueNoThrow<Impl>()) {}
StatsUploadClient::~StatsUploadClient() = default;
void StatsUploadClient::close() {
  if (impl) impl->http.end();
}

bool StatsUploadClient::deviceId(char* out, const size_t capacity) {
  if (KOReaderSyncClient::readerId(out, capacity)) return true;
  LOG_ERR("StatsSync", "Cannot read device identity");
  return false;
}

StatsUploadClient::Result StatsUploadClient::put(const char* endpoint, const char* payload, const unsigned bookCount,
                                                 const bool daily, const char* sessionId) {
  return putBody(endpoint, payload, bookCount, daily, sessionId, nullptr);
}

StatsUploadClient::Result StatsUploadClient::putFile(const char* endpoint, const char* path, unsigned bookCount) {
  return putBody(endpoint, nullptr, bookCount, false, nullptr, path);
}

namespace {
class StatsRequest final : public SyncLog::Request {
 public:
  using SyncLog::Request::Request;
  StatsUploadClient::Result finish(StatsUploadClient::Result result) {
    using Result = StatsUploadClient::Result;

    SyncLog::Request::finish(result == Result::Ok            ? "ok"
                             : result == Result::Network     ? "connection-failed"
                             : result == Result::Auth        ? "authentication-failed"
                             : result == Result::Unsupported ? "unsupported"
                             : result == Result::LowMemory   ? "low-memory"
                             : result == Result::Skipped     ? "skipped"
                             : result == Result::Server      ? "server-error"
                                                             : "invalid-response");
    return result;
  }
};
}  // namespace

StatsUploadClient::Result StatsUploadClient::putBody(const char* endpoint, const char* payload, unsigned bookCount,
                                                     bool daily, const char* sessionId, const char* filePath) {
  StatsRequest request("PUT", endpoint, lastHttpCode);

  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate HTTP client");
    return request.finish(Result::LowMemory);
  }
  auto& http = impl->http;
  // Retain a successful connection while headroom permits. If TLS is holding
  // needed heap, release it before applying the normal handshake floors.
  if (insufficientTlsHeap()) http.end();
  if (insufficientTlsHeap()) {
    LOG_ERR("StatsSync", "Insufficient memory for upload");
    return request.finish(Result::LowMemory);
  }
  // Do not follow redirects or send Basic credentials: this API only needs
  // the x-auth headers. Reuse the existing KOReader TLS policy.
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + endpoint)) {
    LOG_ERR("StatsSync", "Invalid server URL");
    http.end();
    return request.finish(Result::Network);
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  FsFile source;
  if (filePath && (!Storage.openFileForRead("StatsSync", filePath, source) || !source.size())) {
    LOG_ERR("StatsSync", "Cannot read upload file");
    source.close();
    http.end();
    return request.finish(Result::InvalidResponse);
  }
  ScopedCleanup closeSource{[&] { source.close(); }};
  size_t size = 0;
  bool complete = true;
#ifdef SIMULATOR
  std::string simulated;
  if (filePath) {
    simulated.resize(source.size());
    if (source.read(simulated.data(), simulated.size()) != static_cast<int>(simulated.size())) {
      http.end();
      return request.finish(Result::InvalidResponse);
    }
  }
  const int status = http.sendRequest("PUT", filePath ? simulated : std::string(payload));
  const auto body = http.getString();
  size = body.length();
  complete = size < sizeof(impl->response);
  if (complete) memcpy(impl->response, body.c_str(), size);
#else
  const auto receive = [&](const uint8_t* data, size_t length) {
    if (length >= sizeof(impl->response) - size) return false;
    memcpy(impl->response + size, data, length);
    size += length;
    return true;
  };
  const auto read = [](void* context, size_t offset, uint8_t* data, size_t length) -> size_t {
    auto& file = *static_cast<FsFile*>(context);
    if (!file.seekSet(offset)) return 0;
    const int count = file.read(data, length);
    return count < 0 ? 0 : static_cast<size_t>(count);
  };
  const int status = filePath
                         ? http.sendRequestStream("PUT", source.size(), read, &source, receive)
                         : http.sendRequest("PUT", reinterpret_cast<const uint8_t*>(payload), strlen(payload), receive);
  complete = http.responseComplete();
#endif
  // File-backed uploads release TLS and the SD reader before parsing acknowledgment.
  const bool closed = source.close();
  if (filePath) {
    http.end();
    complete = complete && closed;
  }
  lastHttpCode = status;
  LOG_INF("StatsSync", "PUT %s: HTTP %d", endpoint, status);
  // A missing sessions endpoint on an older Sync server does not invalidate
  // support for the existing stats endpoints. Keep the outbox for an upgrade.
  const bool counterIds = strncmp(endpoint, "/api/v1/stats/counters/", 23) == 0;
  if (!sessionId && !counterIds) recordMissingApi(status);
  auto failure = [&](Result result) {
    http.end();
    return request.finish(result);
  };
  if (status <= 0) return failure(Result::Network);
  if (status == 401) return failure(Result::Auth);
  if (status == 404 || status == 405) {
    // A confirmed stats server must accept the reset-safe counter API. An
    // older server keeps the outbox, but must not turn this upload into Done.
    const bool required = counterIds && KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED;
    return failure(required ? Result::Server : Result::Unsupported);
  }
  if (status != 200) return failure(Result::Server);
  if (!complete) return failure(Result::InvalidResponse);
  impl->response[size] = '\0';
  JsonDocument reply;
  if (deserializeJson(reply, impl->response) || !reply["until"].is<uint64_t>() ||
      (counterIds && (!reply["counter_ids"].is<unsigned>() || reply["counter_ids"].as<unsigned>() != 1)) ||
      (sessionId && (!reply["accepted_session_ids"].is<JsonArray>() || reply["accepted_session_ids"].size() != 1 ||
                     !reply["accepted_session_ids"][0].is<const char*>() ||
                     strcmp(reply["accepted_session_ids"][0].as<const char*>(), sessionId) != 0)) ||
      (bookCount && (!reply["accepted"].is<unsigned>() || reply["accepted"].as<unsigned>() != bookCount)) ||
      (daily && (!reply["accepted_daily"].is<unsigned>() || reply["accepted_daily"].as<unsigned>() != 1))) {
    return failure(Result::InvalidResponse);
  }
  // Only a valid extension reply proves support; a catch-all 200 page does not.
  KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
  return request.finish(Result::Ok);
}

StatsUploadClient::Result StatsUploadClient::get(const char* endpoint, const char* responsePath,
                                                 const size_t maxBytes) {
  StatsRequest request("GET", endpoint, lastHttpCode);

  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate download HTTP client");
    return request.finish(Result::LowMemory);
  }
  auto& http = impl->http;
  http.end();
  if (insufficientTlsHeap()) {
    LOG_ERR("StatsSync", "Insufficient memory for download");
    return request.finish(Result::LowMemory);
  }
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + endpoint)) return request.finish(Result::Network);
  http.addHeader("Accept", "application/json");
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  // Stream escaped JSON to SD instead of reserving its worst-case size during TLS.
  FsFile file;
  if (!Storage.openFileForWrite("StatsSync", responsePath, file)) {
    LOG_ERR("StatsSync", "Cannot open download file");
    http.end();
    return request.finish(Result::InvalidResponse);
  }
  ScopedCleanup cleanup{[&file] { file.close(); }};
  size_t size = 0;
  bool stored = true;
  const auto store = [&](const uint8_t* data, const size_t length) {
    if (length > maxBytes - size || file.write(data, length) != length) {
      stored = false;
      return false;
    }
    size += length;
    return true;
  };
  bool complete;
#ifdef SIMULATOR
  const int status = http.GET();
  const auto& body = http.getString();
  complete = store(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
#else
  const int status = http.GET(store);
  complete = http.responseComplete();
#endif
  http.end();
  lastHttpCode = status;
  LOG_INF("StatsSync", "Download response: HTTP %d", status);
  if (status <= 0) return request.finish(Result::Network);
  if (status == 401) return request.finish(Result::Auth);
  if (status == 404 || status == 405) return request.finish(Result::Unsupported);
  if (status != 200) return request.finish(Result::Server);
  if (!stored || !complete || !file.sync() || !file.close()) {
    LOG_ERR("StatsSync", "Incomplete download file");
    return request.finish(Result::InvalidResponse);
  }
  return request.finish(Result::Ok);
}

StatsUploadClient::Result StatsUploadClient::probe(const bool trustHtml) {
  StatsRequest request("GET", "/api/v1/stats/summary", lastHttpCode);

  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate HTTP client");
    return request.finish(Result::LowMemory);
  }
  auto& http = impl->http;
  if (insufficientTlsHeap()) {
    LOG_ERR("StatsSync", "Insufficient memory for capability probe");
    return request.finish(Result::LowMemory);
  }
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + "/api/v1/stats/summary")) {
    LOG_ERR("StatsSync", "Invalid server URL");
    http.end();
    return request.finish(Result::Network);
  }
  http.addHeader("Accept", "application/json");
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  // A JSON object body separates the summary API from catch-all HTML pages that
  // some proxies return with 200; only its first non-space byte is kept.
  char first = '\0';
#ifdef SIMULATOR
  const int status = http.GET();
  for (const char c : std::string(http.getString().c_str())) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      first = c;
      break;
    }
  }
#else
  const int status = http.GET([&first](const uint8_t* data, size_t length) {
    for (size_t i = 0; first == '\0' && i < length; ++i) {
      if (data[i] != ' ' && data[i] != '\n' && data[i] != '\r' && data[i] != '\t') first = static_cast<char>(data[i]);
    }
    return true;  // Drain the rest of the summary without storing it.
  });
#endif
  http.end();
  lastHttpCode = status;
  LOG_INF("StatsSync", "Capability probe: HTTP %d", status);
  recordMissingApi(status);
  if (status <= 0) return request.finish(Result::Network);
  if (status == 401) return request.finish(Result::Auth);
  if (status == 404 || status == 405) return request.finish(Result::Unsupported);
  if (status != 200) return request.finish(Result::Server);
  if (first != '{') {
    LOG_INF("StatsSync", "Capability probe reply is not the stats API");
    // Mid-sync this may be a captive portal; leave it unknown until Authenticate.
    if (!trustHtml) return request.finish(Result::InvalidResponse);
    KOREADER_STORE.setServerSupport(SyncServerSupport::UNSUPPORTED);
    return request.finish(Result::Unsupported);
  }
  KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
  return request.finish(Result::Ok);
}

StatsUploadClient::Result StatsUploadClient::downloadStreak() {
  StatsRequest request("GET", "/api/v1/stats/streak", lastHttpCode);

  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate streak HTTP client");
    return request.finish(Result::LowMemory);
  }
  auto& http = impl->http;
  // Release a reused TLS session before checking room for the next handshake.
  if (insufficientTlsHeap()) http.end();
  if (insufficientTlsHeap()) {
    LOG_ERR("StatsSync", "Insufficient memory for streak download");
    return request.finish(Result::LowMemory);
  }
  char endpoint[48];
  const int tz = -(int(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  snprintf(endpoint, sizeof(endpoint), "/api/v1/stats/streak?tz=%d", tz);
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + endpoint)) {
    LOG_ERR("StatsSync", "Invalid streak server URL");
    http.end();
    return request.finish(Result::Network);
  }
  http.addHeader("Accept", "application/json");
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  size_t size = 0;
  bool complete = true;
#ifdef SIMULATOR
  const int status = http.GET();
  const auto body = http.getString();
  size = body.length();
  complete = size < sizeof(impl->response);
  if (complete) memcpy(impl->response, body.c_str(), size);
#else
  const int status = http.GET([&](const uint8_t* data, size_t length) {
    if (length >= sizeof(impl->response) - size) return false;
    memcpy(impl->response + size, data, length);
    size += length;
    return true;
  });
  complete = http.responseComplete();
#endif
  http.end();
  lastHttpCode = status;
  LOG_INF("StatsSync", "Streak download: HTTP %d", status);
  // Do not downgrade the existing stats API when just the new endpoint is absent.
  if (status == 404 || status == 405) return request.finish(Result::Skipped);
  if (status <= 0) return request.finish(Result::Network);
  if (status == 401) return request.finish(Result::Auth);
  if (status != 200) return request.finish(Result::Server);
  const auto invalid = [&]() {
    LOG_ERR("StatsSync", "Invalid server reading calendar");
    return request.finish(Result::InvalidResponse);
  };
  if (!complete) return invalid();
  impl->response[size] = '\0';
  JsonDocument reply;
  if (deserializeJson(reply, impl->response) || reply["v"] != 1 || !reply["anchor_day"].is<uint32_t>() ||
      reply["anchor_day"].as<uint32_t>() == 0 || reply["anchor_day"].as<uint32_t>() > 36524 ||
      !reply["streak"].is<uint16_t>() || !reply["history_b64"].is<const char*>()) {
    return invalid();
  }
  const char* encoded = reply["history_b64"].as<const char*>();
  if (strlen(encoded) != 124 || encoded[123] != '=') return invalid();
  GlobalReadingStats streak;
  size_t decoded = 0;
  if (mbedtls_base64_decode(streak.readingHistoryBits.data(), READING_HISTORY_BYTES, &decoded,
                            reinterpret_cast<const unsigned char*>(encoded), 124) != 0 ||
      decoded != READING_HISTORY_BYTES || (streak.readingHistoryBits.back() & 0xfc) != 0) {
    return invalid();
  }
  streak.readingHistoryAnchorDay = reply["anchor_day"].as<uint32_t>();
  streak.longestReadingStreak = reply["streak"].as<uint16_t>();
  return request.finish(GlobalReadingStats::saveServerStreak(streak) ? Result::Ok : Result::InvalidResponse);
}

const char* StatsUploadClient::resultString(Result result) {
  if (result == Result::Ok) return tr(STR_DONE);
  if (result == Result::Skipped) return tr(STR_NONE_OPT);
  if (result == Result::Unsupported) return tr(STR_NOT_SUPPORTED);
  return errorString(result);
}

const char* StatsUploadClient::errorString(Result result) {
  switch (result) {
    case Result::Ok:
      return "";
    case Result::Auth:
      return tr(STR_AUTH_FAILED);
    case Result::Unsupported:
      return tr(STR_KOREADER_SYNC_HTTP_404);
    case Result::LowMemory:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    case Result::Network:
      return tr(STR_CONNECTION_FAILED);
    case Result::Server:
      return tr(STR_SYNC_SERVER_ERROR_SHORT);
    case Result::InvalidResponse:
      return tr(STR_SYNC_INVALID_DATA_SHORT);
    default:
      return tr(STR_SYNC_FAILED_MSG);
  }
}

const char* StatsUploadClient::formatResult(char* buffer, const size_t capacity, const Result result,
                                            const int httpCode) {
  const char* text = resultString(result);
  if (result != Result::Ok && result != Result::Skipped && result != Result::Cancelled && httpCode > 0) {
    snprintf(buffer, capacity, "%s (HTTP %d)", text, httpCode);
    return buffer;
  }
  return text;
}
