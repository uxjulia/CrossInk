#pragma once

#include <cstdint>

#ifndef TEST_WIFI_BOARD_LIMIT
#define TEST_WIFI_BOARD_LIMIT 0
#endif

namespace BoardConfig {
struct BoardProfile {
  uint8_t wifiMaxTxPower;
};
inline BoardProfile ACTIVE{TEST_WIFI_BOARD_LIMIT};
}  // namespace BoardConfig
