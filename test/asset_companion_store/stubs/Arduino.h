#pragma once
#define HEX 16
inline void delay(unsigned) {}

inline unsigned long millis() {
  static unsigned long tick = 0;
  return tick += 20;
}
