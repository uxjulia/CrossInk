#include <BoardConfig.h>
#include <Logging.h>
#include <WifiTransmitPower.h>
#include <esp_wifi.h>

#include <algorithm>

namespace {
// Remember the original ceiling across radio stop/start cycles: ESP-IDF can
// retain our lower limit even after WiFi.mode(WIFI_OFF).
std::atomic<int8_t> defaultTransmitPower{-1};

esp_err_t stopAfterPowerFailure(const esp_err_t error) {
  LOG_ERR("WIFI", "Could not apply transmit power limit: %d", static_cast<int>(error));
  const esp_err_t stopped = esp_wifi_stop();
  if (stopped != ESP_OK) LOG_ERR("WIFI", "Could not stop WiFi after power configuration failure: %d", int(stopped));
  return error;
}
}  // namespace

extern "C" esp_err_t __real_esp_wifi_start();

extern "C" esp_err_t __wrap_esp_wifi_start() {
  const esp_err_t started = __real_esp_wifi_start();
  if (started != ESP_OK) return started;

  const uint8_t requested = WifiTransmitPower::getLimit();
  const uint8_t boardLimit = BoardConfig::ACTIVE.wifiMaxTxPower;
  int8_t original = defaultTransmitPower.load(std::memory_order_relaxed);
  // Uncapped boards keep the untouched Default path free of extra driver calls.
  if (requested == 0 && boardLimit == 0 && original < 0) return ESP_OK;

  if (original < 0) {
    int8_t current = 0;
    const esp_err_t read = esp_wifi_get_max_tx_power(&current);
    if (read != ESP_OK) return stopAfterPowerFailure(read);
    if (current < 8 || current > 84) return stopAfterPowerFailure(ESP_ERR_INVALID_ARG);
    defaultTransmitPower.compare_exchange_strong(original, current, std::memory_order_relaxed);
    original = defaultTransmitPower.load(std::memory_order_relaxed);
  }

  // Honor the board ceiling even on Default; a preference can only lower it.
  const int8_t ceiling = boardLimit == 0 ? original : std::min(original, static_cast<int8_t>(boardLimit));
  const int8_t limit = requested == 0 ? ceiling : std::min(ceiling, static_cast<int8_t>(requested));
  const esp_err_t applied = esp_wifi_set_max_tx_power(limit);
  if (applied != ESP_OK) return stopAfterPowerFailure(applied);
  LOG_INF("WIFI", "Transmit power limit: %d quarter-dBm%s", int(limit), requested == 0 ? " (default restored)" : "");
  return ESP_OK;
}
