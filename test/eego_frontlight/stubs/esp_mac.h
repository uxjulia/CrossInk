#pragma once
#include <cstdint>
constexpr int ESP_OK = 0;
inline uint32_t unitUid = 0xBB0409;
inline int esp_efuse_mac_get_default(uint8_t* mac) {
  mac[0] = mac[1] = mac[2] = 0;
  mac[3] = unitUid >> 16;
  mac[4] = unitUid >> 8;
  mac[5] = unitUid;
  return ESP_OK;
}
