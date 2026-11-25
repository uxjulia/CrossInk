#pragma once
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define IRAM_ATTR
constexpr int HIGH = 1, LOW = 0, INPUT = 1, OUTPUT = 3, INPUT_PULLUP = 5, INPUT_PULLDOWN = 6, FALLING = 2, ADC_11db = 3;
inline unsigned long fakeNow = 1000;
inline int gpioLevels[49] = {};
inline int criticalDepth = 0;
inline void (*beforeInputRead)() = nullptr;
inline void (*touchIsr)() = nullptr;
inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) { gpioLevels[p] = v; }
inline int digitalRead(int p) {
  if (beforeInputRead) beforeInputRead();
  return gpioLevels[p];
}
inline void delay(unsigned long n) { fakeNow += n; }
inline void delayMicroseconds(unsigned int) {}
inline unsigned long millis() { return fakeNow; }
inline void analogSetAttenuation(int) {}
inline int analogReadMilliVolts(int) { return 3300; }
inline int adcLevels[49] = {};
inline int adcReads = 0;
inline int analogRead(int pin) {
  if (beforeInputRead) beforeInputRead();
  ++adcReads;
  return adcLevels[pin];
}
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*f)(), int) { touchIsr = f; }
inline void detachInterrupt(int) { touchIsr = nullptr; }
struct SerialStub {
  explicit operator bool() const { return false; }
};
inline SerialStub Serial;

using HardwareSerial = SerialStub;
inline HardwareSerial Serial0;
