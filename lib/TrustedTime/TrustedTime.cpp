#include "TrustedTime.h"

#include <Arduino.h>
#ifdef SIMULATOR
#include <freertos/FreeRTOS.h>
#endif
#include <Logging.h>
#ifndef SIMULATOR
#include <Preferences.h>
// Suppress ESP-IDF's legacy inline aliases so the core-locked handoff below
// can call the raw, synchronous lwIP sntp_stop rather than its queued wrapper.
#define ESP_NETIF_COMPONENT_BUILD
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#undef SNTP_OPMODE_POLL
#include <lwip/apps/sntp.h>
#include <lwip/dns.h>
#undef ESP_NETIF_COMPONENT_BUILD
#else
#include <esp_sntp.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace trustedtime {

namespace {

// Below this the clock was obviously never set (2025-01-01 UTC).
constexpr int64_t MIN_VALID_EPOCH = 1735689600LL;
// Above this a value is a fault, not a date (2100-01-01 UTC). Every supported
// RTC chip stops at 2099, so anything past this is a dead battery or a corrupt read --
// and the floor never lowers again, so adopting one would lock every real clock
// out from then on.
constexpr int64_t MAX_VALID_EPOCH = 4102444800LL;
// How far ahead of the persisted floor an RTC may read. A chip whose calendar is
// a year past what we have already trusted is faulty rather than right, and the
// days either side cover a long power-off: past this the clock stays on the
// floor until SNTP answers.
constexpr int64_t MAX_RTC_LEAD_SECS = 31536000LL;
// NVS wear guard: only rewrite the floor when it moved by at least this much.
constexpr int64_t MIN_ADVANCE_SECS = 60;

constexpr const char* PREFS_NAMESPACE = "cptime";
constexpr const char* PREFS_KEY = "floor";

// Latest time this boot has seen (or restored from NVS). trustedNow() never
// reports earlier, so a backward clock step (a bad SNTP answer, a manual set)
// cannot reopen an expired loan. Updated from the SNTP callback's lwIP task as
// well as the main task; 64-bit atomics are not lock-free on the C3.
std::mutex persistedFloorLock;
// Background sync and foreground RTC/stats sync share the one lwIP SNTP client.
// Separate from the floor lock: the lwIP callback must persist time while we wait.
std::mutex syncLock;
int64_t ramFloor = 0;
portMUX_TYPE ramFloorLock = portMUX_INITIALIZER_UNLOCKED;

// Raises the in-RAM floor to `value` if higher; returns the resulting floor.
int64_t raiseFloor(const int64_t value) {
  portENTER_CRITICAL(&ramFloorLock);
  if (value > ramFloor) ramFloor = value;
  const int64_t floor = ramFloor;
  portEXIT_CRITICAL(&ramFloorLock);
  return floor;
}

#ifndef SIMULATOR
int64_t readFloor() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true)) return 0;
  const int64_t value = prefs.getLong64(PREFS_KEY, 0);
  prefs.end();
  return value;
}

void writeFloor(const int64_t value) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putLong64(PREFS_KEY, value);
  prefs.end();
}

#else
// Host wall time is the simulator clock; never change the computer clock.
int64_t readFloor() { return 0; }
void writeFloor(int64_t) {}
#endif

// SNTP sync callback (lwIP task context; Preferences/NVS is mutex-guarded).
void onTimeSynced(struct timeval*) { note(); }

#ifndef SIMULATOR
esp_err_t clearDnsCache(void*) {
  dns_clear_cache();
  return ESP_OK;
}

esp_err_t stopBackgroundSntp(void*) {
  // A pending SNTP DNS callback must run under the lwIP core lock before stop.
  dns_clear_cache();
  sntp_stop();
  return ESP_OK;
}
#endif

void configureSntp() {
  sntp_set_time_sync_notification_cb(&onTimeSynced);
  // SNTP uses UTC epochs regardless of the display timezone. Copy TZ before
  // configTzTime replaces the environment entry.
  const char* currentTz = getenv("TZ");
  char timezone[64];
  snprintf(timezone, sizeof(timezone), "%s", currentTz ? currentTz : "UTC0");
#ifndef SIMULATOR
  configTzTime(timezone, "pool.ntp.org");
#endif
}

}  // namespace

void init() {
  std::lock_guard<std::mutex> lock(persistedFloorLock);
  sntp_set_time_sync_notification_cb(&onTimeSynced);
  const int64_t floor = readFloor();
  if (floor < MIN_VALID_EPOCH) return;
  raiseFloor(floor);
  if (static_cast<int64_t>(time(nullptr)) < floor) {
    // Cold boot reset the clock; resume from the floor so time keeps moving
    // forward across power cycles instead of restarting at epoch 0.
    timeval tv = {static_cast<time_t>(floor), 0};
#ifndef SIMULATOR
    settimeofday(&tv, nullptr);
#endif
    LOG_DBG("TIME", "Clock restored to persisted floor");
  }
}

void note() {
  std::lock_guard<std::mutex> lock(persistedFloorLock);
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (now < MIN_VALID_EPOCH) return;
  raiseFloor(now);
  if (now - readFloor() >= MIN_ADVANCE_SECS) writeFloor(now);
}

void adopt(const int64_t epoch) {
  std::lock_guard<std::mutex> lock(persistedFloorLock);
  // Out of range: leave the clock and the floor exactly as they are.
  if (epoch < MIN_VALID_EPOCH || epoch > MAX_VALID_EPOCH) return;
  // Compared against the persisted floor, not the in-RAM one: boot calls this
  // before init(), so the in-RAM floor is still empty and the persisted value is
  // the only record of a time this device has already lived through.
  const int64_t stored = readFloor();
  if (stored >= MIN_VALID_EPOCH && epoch - stored > MAX_RTC_LEAD_SECS) return;
  // Deep sleep keeps the system clock, so a wake arrives with a live clock and
  // an empty floor while the RTC may be the older reading: whichever is newer
  // wins, and only inside the supported window.
  const int64_t now = static_cast<int64_t>(time(nullptr));
  const bool nowPlausible = now >= MIN_VALID_EPOCH && now <= MAX_VALID_EPOCH;
  const int64_t candidate = nowPlausible && now > epoch ? now : epoch;
  if (candidate < raiseFloor(0)) return;  // never rewind below a time we already trust
  raiseFloor(candidate);
  timeval tv = {static_cast<time_t>(candidate), 0};
#ifndef SIMULATOR
  settimeofday(&tv, nullptr);
#endif
  if (candidate - readFloor() >= MIN_ADVANCE_SECS) writeFloor(candidate);
}

void startSync() {
  std::lock_guard<std::mutex> lock(syncLock);
  sntp_set_time_sync_notification_cb(&onTimeSynced);
  if (esp_sntp_enabled()) return;  // running; the sync callback handles the rest
  configureSntp();
}

bool syncNow(const uint32_t timeoutMs) {
  std::lock_guard<std::mutex> lock(syncLock);
#ifndef SIMULATOR
  // esp-netif deinit cannot stop a client started by Arduino configTzTime.
  // Explicitly hand off the background client before creating the wait service.
  const esp_err_t stopResult = esp_netif_tcpip_exec(stopBackgroundSntp, nullptr);
  if (stopResult != ESP_OK) {
    LOG_ERR("TIME", "Failed to stop background NTP: %s", esp_err_to_name(stopResult));
    return false;
  }
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  config.sync_cb = &onTimeSynced;
  const esp_err_t initResult = esp_netif_sntp_init(&config);
  if (initResult != ESP_OK) {
    LOG_ERR("TIME", "Failed to start NTP sync: %s", esp_err_to_name(initResult));
    return false;
  }
  const esp_err_t syncResult = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeoutMs));
  // Retain HalClock's timeout cleanup: a later hostname lookup must not run a
  // pending SNTP DNS callback from an application task while clearing the cache.
  const esp_err_t clearResult = esp_netif_tcpip_exec(clearDnsCache, nullptr);
  esp_netif_sntp_deinit();
  if (clearResult != ESP_OK || syncResult != ESP_OK) {
    LOG_ERR("TIME", "NTP sync failed: sync=%s DNS=%s", esp_err_to_name(syncResult), esp_err_to_name(clearResult));
    return false;
  }
  note();
  return true;
#else
  // configureSntp() (configTzTime) restarts SNTP if already running.
  configureSntp();
  const unsigned long deadline = millis() + timeoutMs;
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && static_cast<long>(deadline - millis()) > 0) {
    delay(100);
  }
  const bool synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  if (synced) note();
  return synced;
#endif
}

int64_t trustedNow() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  // Never earlier than a time already seen; 0 while neither is trustworthy.
  const int64_t floor = raiseFloor(now >= MIN_VALID_EPOCH ? now : 0);
  return floor >= MIN_VALID_EPOCH ? floor : 0;
}

}  // namespace trustedtime
