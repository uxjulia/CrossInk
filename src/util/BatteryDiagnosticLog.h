#pragma once

#include <HalPowerManager.h>  // CROSSINK_BATTERY_DIAG_LOG

#include <cstdint>

// Repeated from HalPowerManager.h rather than relied upon: simulator builds
// ignore lib/hal and pick up the crossink-simulator stand-in instead, which
// does not define this. Both guards are #ifndef, so they cannot disagree.
#ifndef CROSSINK_BATTERY_DIAG_LOG
#define CROSSINK_BATTERY_DIAG_LOG 0
#endif

// Battery telemetry log, for diagnosing a fuel gauge that misreports state of
// charge. The X3's BQ27220 is read raw - no calibration, no cross-check - so a
// device can sit at a confident 87% and then die.
//
// Serial logging cannot capture this. Any USB connection powers the device, and
// on the X3 (which has no charger IC) it also flips the sign of the gauge's
// current register, which is exactly what isCharging() infers charge state
// from. Observing a real discharge means writing to the SD card instead.
//
// Rows are appended to /battery_log.csv as:
//   timestamp,uptime_ms,soc,mv,charging,event,version,git_sha,git_dirty,device,wake_route
// A field that could not be read is written empty rather than as a plausible
// zero. The timestamp is local wall-clock at minute resolution - HalClock does
// not expose the RTC's seconds - so use uptime_ms to order rows within one
// wake. The first firmware using this schema preserves an old-format log as
// /battery_log_v1.csv instead of appending unlabeled columns to it.
//
// Enable with -DCROSSINK_BATTERY_DIAG_LOG=1. The debug environment carries a
// commented opt-in flag; no shipping environment enables it. Firmware only -
// the simulator's stand-in HAL has no battery backend to sample, so the flag
// must stay off there.
namespace BatteryDiagnosticLog {

// The device sleeps through most of a discharge, and the X3 cuts the SD rail
// while it does, so samples are only possible on either side of that gap.
// Recording both ends brackets each sleep interval, which is what separates
// drain while reading from drain while idle.
enum class Event : uint8_t {
  Wake,  // any startup that reaches a mounted SD card, cold boot included
  Sleep,
};

#if CROSSINK_BATTERY_DIAG_LOG
// Appends one sample. Never fatal: every failure logs and returns, because a
// diagnostic must not be able to take down the boot or sleep path it sits in.
// deviceName is the runtime board profile name. wakeRoute applies only to a
// Wake event; pass nullptr for Sleep.
void record(Event event, const char* deviceName, const char* wakeRoute = nullptr);
#else
inline void record(Event, const char* = nullptr, const char* = nullptr) {}
#endif

}  // namespace BatteryDiagnosticLog
