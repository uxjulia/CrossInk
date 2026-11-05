#pragma once
#include <cstddef>
struct KOReaderSyncClient {
  static bool readerId(char*, size_t) { return false; }
};
template <class Client, class Headers>
void applyCustomHttpHeaders(Client&, const Headers&) {}
