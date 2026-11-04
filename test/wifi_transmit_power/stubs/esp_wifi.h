#pragma once
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;

extern "C" {
esp_err_t esp_wifi_get_max_tx_power(int8_t* power);
esp_err_t esp_wifi_set_max_tx_power(int8_t power);
esp_err_t esp_wifi_stop();
}
