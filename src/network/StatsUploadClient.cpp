#include "StatsUploadClient.h"

#include <ArduinoJson.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <mbedtls/base64.h>
#ifndef SIMULATOR
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "activities/reader/GlobalReadingStats.h"

namespace {
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

bool StatsUploadClient::deviceId(char* out, const size_t capacity) {
  if (KOReaderSyncClient::readerId(out, capacity)) return true;
  LOG_ERR("StatsSync", "Cannot read device identity");
  return false;
}

StatsUploadClient::Result StatsUploadClient::put(const char* endpoint, const char* payload, const bool book,
                                                 const bool daily, const char* sessionId) {
  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate HTTP client");
    return Result::LowMemory;
  }
  auto& http = impl->http;
  // Retain a successful connection while headroom permits. If TLS is holding
  // needed heap, release it before applying the normal handshake floors.
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000) http.end();
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000) {
    LOG_ERR("StatsSync", "Insufficient memory for upload");
    return Result::LowMemory;
  }
  // Do not follow redirects or send Basic credentials: this API only needs
  // the x-auth headers. Reuse the existing KOReader TLS policy.
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + endpoint)) {
    LOG_ERR("StatsSync", "Invalid server URL");
    http.end();
    return Result::Network;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  applyCustomHttpHeaders(http, KOREADER_STORE.getCustomHeaders());
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  size_t size = 0;
  bool complete = true;
#ifdef SIMULATOR
  const int status = http.sendRequest("PUT", std::string(payload));
  const auto body = http.getString();
  size = body.length();
  complete = size < sizeof(impl->response);
  if (complete) memcpy(impl->response, body.c_str(), size);
#else
  const int status = http.sendRequest("PUT", reinterpret_cast<const uint8_t*>(payload), strlen(payload),
                                      [&](const uint8_t* data, size_t length) {
                                        if (length >= sizeof(impl->response) - size) return false;
                                        memcpy(impl->response + size, data, length);
                                        size += length;
                                        return true;
                                      });
  complete = http.responseComplete();
#endif
  LOG_INF("StatsSync", "Upload response: HTTP %d", status);
  // A missing sessions endpoint on an older Sync server does not invalidate
  // support for the existing stats endpoints. Keep the outbox for an upgrade.
  if (!sessionId) recordMissingApi(status);
  auto failure = [&](Result result) {
    http.end();
    return result;
  };
  if (status <= 0) return failure(Result::Network);
  if (status == 401) return failure(Result::Auth);
  if (status == 404 || status == 405) return failure(Result::Unsupported);
  if (status != 200) return failure(Result::Server);
  if (!complete) return failure(Result::InvalidResponse);
  impl->response[size] = '\0';
  JsonDocument reply;
  if (deserializeJson(reply, impl->response) || !reply["until"].is<uint64_t>() ||
      (sessionId && (!reply["accepted_session_ids"].is<JsonArray>() || reply["accepted_session_ids"].size() != 1 ||
                     !reply["accepted_session_ids"][0].is<const char*>() ||
                     strcmp(reply["accepted_session_ids"][0].as<const char*>(), sessionId) != 0)) ||
      (book && (!reply["accepted"].is<unsigned>() || reply["accepted"].as<unsigned>() != 1)) ||
      (daily && (!reply["accepted_daily"].is<unsigned>() || reply["accepted_daily"].as<unsigned>() != 1))) {
    return failure(Result::InvalidResponse);
  }
  // Only a valid extension reply proves support; a catch-all 200 page does not.
  KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
  return Result::Ok;
}

StatsUploadClient::Result StatsUploadClient::probe(const bool trustHtml) {
  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate HTTP client");
    return Result::LowMemory;
  }
  auto& http = impl->http;
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000) {
    LOG_ERR("StatsSync", "Insufficient memory for capability probe");
    return Result::LowMemory;
  }
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + "/api/v1/stats/summary")) {
    LOG_ERR("StatsSync", "Invalid server URL");
    http.end();
    return Result::Network;
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
  LOG_INF("StatsSync", "Capability probe: HTTP %d", status);
  recordMissingApi(status);
  if (status <= 0) return Result::Network;
  if (status == 401) return Result::Auth;
  if (status == 404 || status == 405) return Result::Unsupported;
  if (status != 200) return Result::Server;
  if (first != '{') {
    LOG_INF("StatsSync", "Capability probe reply is not the stats API");
    // Mid-sync this may be a captive portal; leave it unknown until Authenticate.
    if (!trustHtml) return Result::InvalidResponse;
    KOREADER_STORE.setServerSupport(SyncServerSupport::UNSUPPORTED);
    return Result::Unsupported;
  }
  KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
  return Result::Ok;
}

StatsUploadClient::Result StatsUploadClient::downloadStreak() {
  if (!impl) {
    LOG_ERR("StatsSync", "Cannot allocate streak HTTP client");
    return Result::LowMemory;
  }
  auto& http = impl->http;
  // Release a reused TLS session before checking room for the next handshake.
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000) http.end();
  if (ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000) {
    LOG_ERR("StatsSync", "Insufficient memory for streak download");
    return Result::LowMemory;
  }
  char endpoint[48];
  const int tz = -(int(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  snprintf(endpoint, sizeof(endpoint), "/api/v1/stats/streak?tz=%d", tz);
  http.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(KOREADER_STORE.getBaseUrl() + endpoint)) {
    LOG_ERR("StatsSync", "Invalid streak server URL");
    http.end();
    return Result::Network;
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
  LOG_INF("StatsSync", "Streak download: HTTP %d", status);
  // Do not downgrade the existing stats API when just the new endpoint is absent.
  if (status == 404 || status == 405) return Result::Skipped;
  if (status <= 0) return Result::Network;
  if (status == 401) return Result::Auth;
  if (status != 200) return Result::Server;
  const auto invalid = []() {
    LOG_ERR("StatsSync", "Invalid server reading calendar");
    return Result::InvalidResponse;
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
  return GlobalReadingStats::saveServerStreak(streak) ? Result::Ok : Result::InvalidResponse;
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
    default:
      return tr(STR_SYNC_FAILED_MSG);
  }
}
