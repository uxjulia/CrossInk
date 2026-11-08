#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Zero identifies an existing legacy counter set. Fresh counters get an ID once
// at creation; copies, retries and restored snapshots retain that same ID.
struct StatsCounterId {
  std::array<uint8_t, 16> bytes{};
  static StatsCounterId create();
  void hex(char (&out)[33]) const {
    constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
      out[i * 2] = HEX[bytes[i] >> 4];
      out[i * 2 + 1] = HEX[bytes[i] & 15];
    }
    out[32] = 0;
  }
};
