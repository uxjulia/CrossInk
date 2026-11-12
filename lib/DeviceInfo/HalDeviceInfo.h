#pragma once

#include <cstdint>

// Read-only hardware boundary. The exportable snapshot excludes identifiers, network state, settings and book data.
namespace HalDeviceInfo {
enum class Presence : uint8_t { Absent, Available, Unavailable, Simulated };
struct Snapshot {
  const char* device = nullptr;
  const char* chip = nullptr;
  const char* displayController = nullptr;
  const char* touchController = nullptr;
  const char* sdk = nullptr;
  uint32_t uptimeSeconds = 0;
  uint32_t flashBytes = 0;
  uint32_t internalFree = 0;
  uint32_t internalMinimum = 0;
  uint32_t internalLargest = 0;
  uint32_t psramTotal = 0;
  uint32_t psramFree = 0;
  uint32_t psramLargest = 0;
  uint64_t sdBytes = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t cpuMHz = 0;
  uint16_t chipRevision = 0;
  uint8_t cores = 0;
  uint8_t resetReason = 0;
  bool simulated = false;
  bool sdmmc = false;
  bool sdReady = false;
  Presence psram = Presence::Absent;
  Presence touch = Presence::Absent;
  Presence frontlight = Presence::Absent;
  Presence rtc = Presence::Absent;
  Presence imu = Presence::Absent;
};
Snapshot capture();
// On-screen EEGO identity only; intentionally separate from the exportable snapshot.
// The caller provides nine bytes for up to eight decimal digits plus the terminator.
bool readEegoUid(char (&uid)[9]);
}  // namespace HalDeviceInfo
