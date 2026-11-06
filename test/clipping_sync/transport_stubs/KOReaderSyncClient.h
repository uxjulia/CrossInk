#pragma once
#include <cstddef>
#include <cstdint>
struct KOReaderSyncClient {
  static constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
  static constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;
  static bool readerId(char*, size_t) { return false; }
};
template <class Client, class Headers>
void applyCustomHttpHeaders(Client&, const Headers&) {}
