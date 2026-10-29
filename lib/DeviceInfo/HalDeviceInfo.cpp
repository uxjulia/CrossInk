#include "HalDeviceInfo.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>

#ifndef SIMULATOR
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace HalDeviceInfo {
namespace {
#ifndef SIMULATOR
const char* touchName(BoardConfig::TouchController controller) {
  switch (controller) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
    case BoardConfig::TouchController::Cst816s:
      return "CST816S";
  }
  return nullptr;
}
Presence presence(bool configured, bool available) {
  return !configured ? Presence::Absent : available ? Presence::Available : Presence::Unavailable;
}
#endif
}  // namespace

Snapshot capture() {
  Snapshot info;
  info.device = BoardConfig::ACTIVE.name;
  info.uptimeSeconds = millis() / 1000;
  info.width = display.getDisplayWidth();
  info.height = display.getDisplayHeight();
  info.sdReady = Storage.ready();

#ifdef SIMULATOR
  info.simulated = true;
  info.sdmmc = FREEINK_SD_SDMMC;
  info.touch = gpio.hasTouch() ? Presence::Simulated : Presence::Absent;
  info.frontlight = Frontlight.present() ? Presence::Simulated : Presence::Absent;
  info.rtc = halClock.isAvailable() ? Presence::Simulated : Presence::Absent;
  info.imu = halTiltSensor.isAvailable() ? Presence::Simulated : Presence::Absent;
#else
  if (info.sdReady) info.sdBytes = Storage.totalBytes();  // No expensive FAT free-space scan.
  const auto& board = BoardConfig::ACTIVE;
  info.displayController = display.getControllerName();
  info.touchController = touchName(board.touch.controller);
  info.sdmmc = board.sdmmc.busWidth != 0;
  info.touch = presence(board.touch.controller != BoardConfig::TouchController::None, gpio.hasTouch());
  info.frontlight = Frontlight.present() ? Presence::Available : Presence::Absent;
  info.rtc = presence(board.sensors.rtcType != BoardConfig::RtcType::None, halClock.isAvailable());
  info.imu = presence(board.sensors.imuType != BoardConfig::ImuType::None, halTiltSensor.isAvailable());
  info.chip = ESP.getChipModel();
  info.chipRevision = ESP.getChipRevision();
  info.cores = ESP.getChipCores();
  info.cpuMHz = ESP.getCpuFreqMHz();
  info.flashBytes = ESP.getFlashChipSize();
  info.sdk = ESP.getSdkVersion();
  info.resetReason = static_cast<uint8_t>(esp_reset_reason());
  // 8-bit internal heap excludes PSRAM, unlike combined heap figures on S3.
  constexpr uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  info.internalFree = heap_caps_get_free_size(internalCaps);
  info.internalMinimum = heap_caps_get_minimum_free_size(internalCaps);
  info.internalLargest = heap_caps_get_largest_free_block(internalCaps);
  info.psramTotal = ESP.getPsramSize();
#ifdef BOARD_HAS_PSRAM
  info.psram = presence(true, info.psramTotal != 0);
#else
  info.psram = presence(false, false);
#endif
  if (info.psramTotal) {
    info.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    info.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
#endif
  return info;
}
}  // namespace HalDeviceInfo
