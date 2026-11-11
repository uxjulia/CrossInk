#pragma once
#include <Arduino.h>

#include <cassert>
#include <vector>
struct WireStub {
  std::array<uint8_t, 256> regs{};
  std::vector<uint8_t> tx;
  uint8_t address = 0, pointer = 0;
  bool fitted = true;
  unsigned writes = 0;
  int failRegister = -1;
  void begin(int sda, int scl, uint32_t hz) { assert(sda == 2 && scl == 1 && hz == 400000); }
  void setTimeOut(unsigned) {}
  void beginTransmission(uint8_t addr) {
    address = addr;
    tx.clear();
  }
  void write(uint8_t value) { tx.push_back(value); }
  int endTransmission(bool = true) {
    if (!fitted || address != 0x36 || !pins[12]) return 2;
    if (tx.empty()) return 0;
    pointer = tx[0];
    if (tx.size() == 1) return 0;
    if (pointer == failRegister) {
      failRegister = -1;
      return 2;
    }
    regs[pointer] = tx[1];
    ++writes;
    return 0;
  }
  uint8_t requestFrom(uint8_t addr, uint8_t n, uint8_t) { return fitted && pins[12] && addr == 0x36 ? n : 0; }
  uint8_t read() { return regs[pointer]; }
};
inline WireStub Wire;
