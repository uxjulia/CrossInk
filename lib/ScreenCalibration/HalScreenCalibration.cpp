#include "HalScreenCalibration.h"

#include <Logging.h>

#include <cstddef>

#ifdef SIMULATOR
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#else
#include <nvs.h>
#endif

namespace {
// Explicit byte encoding: independent of compiler padding and enum layout.
constexpr size_t LEGACY_RECORD_SIZE = 8;
constexpr size_t RECORD_SIZE = 10;
constexpr uint8_t RECORD_VERSION = 2;
void encode(const ScreenInsets& insets, uint8_t (&record)[RECORD_SIZE]) {
  record[0] = 'S';
  record[1] = 'C';
  record[2] = RECORD_VERSION;
  for (size_t i = 0; i < 4; ++i) record[i + 3] = insets.edges[i];
  record[7] = insets.cornerInsets[0];
  record[8] = insets.cornerInsets[1];
  record[9] = 0;
  for (size_t i = 0; i < 9; ++i) record[9] ^= record[i];
}
bool decode(const uint8_t* record, size_t size, ScreenInsets& result) {
  const bool legacy = size == LEGACY_RECORD_SIZE && record[2] == 1;
  if (!legacy && !(size == RECORD_SIZE && record[2] == RECORD_VERSION)) return false;
  uint8_t checksum = 0;
  for (size_t i = 0; i < size; ++i) checksum ^= record[i];
  if (record[0] != 'S' || record[1] != 'C' || checksum != 0) return false;
  for (size_t i = 0; i < 4; ++i) result.edges[i] = record[i + 3];
  // Legacy records keep their edges and inherit device corner defaults.
  if (!legacy) result.cornerInsets = {{record[7], record[8]}};
  return result.valid();
}
#ifdef SIMULATOR
const char* storagePath() {
  const char* overridePath = std::getenv("CROSSINK_CALIBRATION_PATH");
  return overridePath && *overridePath ? overridePath : ".screen-calibration.nvs";
}
#else
constexpr const char* NAMESPACE = "screen-cal";
constexpr const char* KEY = "bounds";
#endif
}  // namespace

ScreenInsets HalScreenCalibration::defaults() {
  // CrossInk's initial calibration is app policy, independent of SDK board
  // geometry. Saved device calibration continues to take precedence.
#if defined(SIMULATOR)
  return ScreenInsets{};
#elif defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4
  return ScreenInsets{{{10, 10, 10, 10}}};
#elif (defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO) || \
    (defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC)
  return ScreenInsets{{{9, 7, 3, 7}}};
#else
  return ScreenInsets{};
#endif
}

PanelLayout HalScreenCalibration::panelLayout() {
#if !defined(SIMULATOR) && defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4
  // Provisional A4 UI clearances belong to CrossInk, where they can be tuned
  // without changing the layout policy of other SDK consumers.
  PanelLayout layout;
  layout.headerOffsetY = 6;
  layout.statusBarOffsetY = 6;
  // Reader bars use the calibrated edge, just as the bottom bar does.
  layout.statusBarAtCalibratedEdge = true;
  layout.tabBarOffsetY = 18;
  layout.tabBarHeightPct = 80;
  layout.tabBarWidthPct = 80;
  return layout;
#endif
  return {};
}

ScreenInsets HalScreenCalibration::load() {
  ScreenInsets result = defaults();
  uint8_t record[RECORD_SIZE]{};
  size_t size = sizeof(record);
#ifdef SIMULATOR
  FILE* file = std::fopen(storagePath(), "rb");
  if (!file) {
    if (errno != ENOENT) LOG_ERR("CAL", "Could not open calibration: %d", errno);
    return result;
  }
  size = std::fread(record, 1, sizeof(record), file);
  const bool complete =
      (size == RECORD_SIZE || size == LEGACY_RECORD_SIZE) && std::fgetc(file) == EOF && !std::ferror(file);
  std::fclose(file);
  if (!complete) {
    LOG_ERR("CAL", "Invalid calibration length; using defaults");
    return result;
  }
#else
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return result;
  if (opened != ESP_OK) {
    LOG_ERR("CAL", "Could not open calibration: %d", opened);
    return result;
  }
  const esp_err_t read = nvs_get_blob(handle, KEY, record, &size);
  nvs_close(handle);
  if (read == ESP_ERR_NVS_NOT_FOUND) return result;
  if (read != ESP_OK || (size != RECORD_SIZE && size != LEGACY_RECORD_SIZE)) {
    LOG_ERR("CAL", "Could not read calibration: %d", read);
    return result;
  }
#endif
  if (!decode(record, size, result)) {
    LOG_ERR("CAL", "Invalid calibration record; using defaults");
    return defaults();
  }
  return result;
}

bool HalScreenCalibration::save(const ScreenInsets& insets) {
  if (!insets.valid()) {
    LOG_ERR("CAL", "Refusing invalid calibration");
    return false;
  }
  uint8_t record[RECORD_SIZE]{};
  encode(insets, record);
#ifdef SIMULATOR
  // Replace atomically so a failed save preserves the previous record.
  const char* path = storagePath();
  char temporary[256];
  const int length = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(temporary)) {
    LOG_ERR("CAL", "Calibration path too long");
    return false;
  }
  FILE* file = std::fopen(temporary, "wb");
  if (!file) {
    LOG_ERR("CAL", "Could not open calibration for write");
    return false;
  }
  bool ok = std::fwrite(record, 1, sizeof(record), file) == sizeof(record);
  if (std::fclose(file) != 0) ok = false;
  if (ok) ok = std::rename(temporary, path) == 0;
  if (!ok) {
    std::remove(temporary);
    LOG_ERR("CAL", "Could not save calibration");
  }
  return ok;
#else
  nvs_handle_t handle;
  esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    LOG_ERR("CAL", "Could not open calibration for write: %d", error);
    return false;
  }
  error = nvs_set_blob(handle, KEY, record, sizeof(record));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) LOG_ERR("CAL", "Could not save calibration: %d", error);
  return error == ESP_OK;
#endif
}
