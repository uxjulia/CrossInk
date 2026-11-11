#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
inline unsigned long fakeNow = 0;
inline unsigned long millis() { return fakeNow; }
inline void delay(unsigned long ms) { fakeNow += ms; }
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(int8_t, uint8_t) {}
inline void ledcDetachPin(int8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}
constexpr int HIGH = 1, LOW = 0, OUTPUT = 3;
inline int Serial = 0;
inline std::array<int, 64> pins{};
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) {
  if (pin >= 0) pins.at(pin) = value;
}
