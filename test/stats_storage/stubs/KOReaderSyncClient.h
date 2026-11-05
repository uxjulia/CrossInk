#pragma once
#include <esp_mac.h>

#include <cstdio>
struct KOReaderSyncClient {
  static bool readerId(char* out, size_t capacity) {
    uint8_t mac[6]{};
    if (esp_efuse_mac_get_default(mac) != 0) return false;
    const int n =
        snprintf(out, capacity, "crossink-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return n > 0 && static_cast<size_t>(n) < capacity;
  }
};
