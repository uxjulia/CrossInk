#pragma once
#include <cstdint>
#include <cstring>
inline uint8_t testMac[6] = {0, 1, 2, 3, 4, 5};
inline bool failMac = false;
inline int esp_efuse_mac_get_default(uint8_t* out) {
  if (failMac) return -1;
  memcpy(out, testMac, 6);
  return 0;
}
