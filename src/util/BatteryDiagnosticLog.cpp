#include "BatteryDiagnosticLog.h"

#if CROSSINK_BATTERY_DIAG_LOG

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "AppVersion.h"
#include "activities/reader/ReadingStatsUtils.h"

namespace BatteryDiagnosticLog {
namespace {

constexpr char LOG_PATH[] = "/battery_log.csv";
constexpr char LEGACY_LOG_HEADER[] = "timestamp,uptime_ms,soc,mv,charging,event\n";
constexpr char V2_LOG_HEADER[] =
    "timestamp,uptime_ms,soc,mv,charging,event,version,git_sha,git_dirty,device,wake_route\n";
constexpr char LOG_HEADER[] =
    "timestamp,uptime_ms,soc,mv,charging,event,version,git_sha,git_dirty,device,wake_route,"
    "display_soc,usb,charge_done,charger_status,estimate_state,settle_ms,sample_age_ms,resumed\n";

// A runaway guard, not an expected operating point: a wake/sleep pair costs
// only a few hundred bytes, so a device cycled 20 times a day takes months to get
// here. Appending stops at the cap so a forgotten diagnostic build cannot fill
// a card.
constexpr size_t MAX_LOG_BYTES = 128u * 1024u;

// "2026-08-25T14:03" plus terminator.
constexpr size_t TIMESTAMP_LEN = 20;
// Sized to hold the full range of the uint16_t fields they format, not the
// range those fields are expected to carry. The SDK clamps state of charge to
// 100 before it reaches here, but that promise lives in another repository, and
// a log built to catch a gauge misreporting itself must record an out-of-spec
// value verbatim rather than silently truncate it into a plausible one.
constexpr size_t SOC_LEN = 6;         // "65535"
constexpr size_t MILLIVOLTS_LEN = 6;  // "65535"
constexpr size_t CHARGING_LEN = 2;    // "0" / "1"
constexpr size_t VERSION_LEN = 64;
constexpr size_t LEGACY_LOG_PATH_LEN = 32;
constexpr size_t HEADER_PROBE_LEN = sizeof(LOG_HEADER) > sizeof(LEGACY_LOG_HEADER) ? sizeof(LOG_HEADER)
                                                                                   : sizeof(LEGACY_LOG_HEADER);
// Main-loop wake/sleep events only. Keep the larger row off the task stack;
// this buffer exists only in explicitly enabled diagnostic builds.
char row[320];
constexpr unsigned MAX_LEGACY_ARCHIVES = 100;

const char* eventName(const Event event) {
  switch (event) {
    case Event::Wake:
      return "wake";
    case Event::Sleep:
      return "sleep";
  }
  return "unknown";
}

// Local wall-clock time, or an empty string when the RTC is absent or invalid.
void formatTimestamp(char* buf, const size_t len) {
  buf[0] = '\0';
  ReadingStatsDateTime now;
  if (!getCurrentLocalReadingStatsDateTime(now)) {
    return;
  }
  snprintf(buf, len, "%04u-%02u-%02uT%02u:%02u", static_cast<unsigned>(now.date.year),
           static_cast<unsigned>(now.date.month), static_cast<unsigned>(now.date.day), static_cast<unsigned>(now.hour),
           static_cast<unsigned>(now.minute));
}

// Writes `value` when the backend could read it, and leaves the field empty
// when it could not, so a failed read never reads back as a real measurement.
void formatOptional(char* buf, const size_t len, const bool known, const unsigned value) {
  if (!known) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, len, "%u", value);
}

void formatText(char* buf, const size_t len, const char* const value) { snprintf(buf, len, "%s", value ? value : ""); }

bool hasHeader(HalFile& file, const char* expectedHeader, const size_t expectedLen) {
  char header[HEADER_PROBE_LEN];
  const int read = file.read(header, expectedLen);
  return read == static_cast<int>(expectedLen) && memcmp(header, expectedHeader, expectedLen) == 0;
}

bool preserveLegacyLog(unsigned version) {
  char archivedPath[LEGACY_LOG_PATH_LEN];
  for (unsigned index = 0; index < MAX_LEGACY_ARCHIVES; ++index) {
    const int pathLen = index == 0
                            ? snprintf(archivedPath, sizeof(archivedPath), "/battery_log_v%u.csv", version)
                            : snprintf(archivedPath, sizeof(archivedPath), "/battery_log_v%u_%u.csv", version, index);
    if (pathLen <= 0 || static_cast<size_t>(pathLen) >= sizeof(archivedPath)) {
      LOG_ERR("BATLOG", "Legacy archive path did not fit");
      return false;
    }
    if (Storage.exists(archivedPath)) continue;
    if (!Storage.rename(LOG_PATH, archivedPath)) {
      LOG_ERR("BATLOG", "Failed to preserve legacy log as %s", archivedPath);
      return false;
    }
    LOG_INF("BATLOG", "Preserved legacy log as %s", archivedPath);
    return true;
  }
  LOG_ERR("BATLOG", "No legacy archive path available for %s", LOG_PATH);
  return false;
}

// Do not append expanded rows to an old header: spreadsheet importers would
// silently put the new values in unlabeled columns. Preserve the original
// diagnostic evidence and begin a clean v3 log instead.
bool prepareLogSchema() {
  if (!Storage.exists(LOG_PATH)) return true;

  HalFile file = Storage.open(LOG_PATH, O_RDONLY);
  if (!file) {
    LOG_ERR("BATLOG", "Failed to inspect %s before append", LOG_PATH);
    return false;
  }
  if (file.size() == 0) {
    file.close();
    return true;
  }

  const bool currentSchema = hasHeader(file, LOG_HEADER, sizeof(LOG_HEADER) - 1);
  unsigned legacyVersion = 0;
  if (!currentSchema) {
    file.seekSet(0);
    if (hasHeader(file, LEGACY_LOG_HEADER, sizeof(LEGACY_LOG_HEADER) - 1)) {
      legacyVersion = 1;
    } else {
      file.seekSet(0);
      if (hasHeader(file, V2_LOG_HEADER, sizeof(V2_LOG_HEADER) - 1)) legacyVersion = 2;
    }
  }
  file.close();

  if (currentSchema) return true;
  if (legacyVersion == 0) {
    LOG_ERR("BATLOG", "%s has an unknown header; refusing to append", LOG_PATH);
    return false;
  }
  return preserveLegacyLog(legacyVersion);
}

}  // namespace

void record(const Event event, const char* const deviceName, const char* const wakeRoute) {
  HalPowerManager::BatteryDiagnostics battery;
  if (!powerManager.getBatteryDiagnostics(battery)) {
    return;  // getBatteryDiagnostics() already logged the reason
  }
  if (!prepareLogSchema()) return;

  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("BATLOG", "Failed to open %s for append", LOG_PATH);
    return;
  }

  const size_t existingBytes = file.size();
  if (existingBytes >= MAX_LOG_BYTES) {
    file.close();
    // Static storage duration despite the inner scope, so the warning is
    // emitted once per boot rather than on every sample.
    static bool capReported = false;
    if (!capReported) {
      capReported = true;
      LOG_ERR("BATLOG", "%s hit the %u byte cap; no longer appending", LOG_PATH, static_cast<unsigned>(MAX_LOG_BYTES));
    }
    return;
  }

  bool ok = true;
  if (existingBytes == 0) {
    constexpr size_t headerLen = sizeof(LOG_HEADER) - 1;
    ok = file.write(LOG_HEADER, headerLen) == headerLen;
  }

  char timestamp[TIMESTAMP_LEN];
  char soc[SOC_LEN];
  char millivolts[MILLIVOLTS_LEN];
  char charging[CHARGING_LEN];
  char version[VERSION_LEN];
  formatTimestamp(timestamp, sizeof(timestamp));
  formatOptional(soc, sizeof(soc), battery.socKnown, battery.soc);
  formatOptional(millivolts, sizeof(millivolts), battery.millivoltsKnown, battery.millivolts);
  formatOptional(charging, sizeof(charging), battery.chargingKnown, battery.charging ? 1u : 0u);
  formatText(version, sizeof(version), AppVersion::version());

  char extra[8][11];  // uint32 maximum plus terminator, including optional fields.
  formatOptional(extra[0], sizeof(extra[0]), battery.displayedKnown, battery.displayedPercent);
  formatOptional(extra[1], sizeof(extra[1]), battery.externalPowerKnown, battery.externalPower);
  formatOptional(extra[2], sizeof(extra[2]), battery.chargeCompleteKnown, battery.chargeComplete);
  formatOptional(extra[3], sizeof(extra[3]), battery.chargerStatus >= 0, battery.chargerStatus);
  formatOptional(extra[4], sizeof(extra[4]), battery.estimateState >= 0 && battery.displayedKnown,
                 battery.estimateState);
  formatOptional(extra[5], sizeof(extra[5]), battery.estimateState >= 0 && battery.displayedKnown, battery.settlingMs);
  formatOptional(extra[6], sizeof(extra[6]), battery.displayedKnown, battery.sampleAgeMs);
  formatOptional(extra[7], sizeof(extra[7]), battery.estimateState >= 0, battery.resumed);
  const int rowLen = snprintf(row, sizeof(row), "%s,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                              timestamp, static_cast<unsigned long>(millis()), soc, millivolts, charging,
                              eventName(event), version, AppVersion::gitSha(), AppVersion::gitDirtyFlag(),
                              deviceName ? deviceName : "", wakeRoute ? wakeRoute : "", extra[0], extra[1], extra[2],
                              extra[3], extra[4], extra[5], extra[6], extra[7]);
  if (rowLen <= 0 || static_cast<size_t>(rowLen) >= sizeof(row)) {
    file.close();
    LOG_ERR("BATLOG", "Battery sample did not fit the row buffer");
    return;
  }
  if (ok) {
    ok = file.write(row, static_cast<size_t>(rowLen)) == static_cast<size_t>(rowLen);
  }

  // close() syncs, which matters on the sleep path: the SD rail is cut shortly
  // after this returns, and the final row is the most interesting one.
  if (!file.close()) {
    ok = false;
  }
  if (!ok) {
    LOG_ERR("BATLOG", "Failed to append battery sample to %s", LOG_PATH);
  }
}

}  // namespace BatteryDiagnosticLog

#endif  // CROSSINK_BATTERY_DIAG_LOG
