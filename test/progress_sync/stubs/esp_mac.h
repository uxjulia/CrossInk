#pragma once
#include <cstdint>
constexpr int ESP_OK = 0;
inline int esp_efuse_mac_get_default(uint8_t*) { return ESP_OK; }
