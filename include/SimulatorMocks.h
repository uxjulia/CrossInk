#pragma once
#include <stdint.h>
#include <unistd.h>

// Mocking Arduino-specific types and functions for the Mac
typedef uint8_t byte;
#define HIGH 0x1
#define LOW 0x0

inline void delay(int ms) { usleep(ms * 1000); }

class SerialMock {
 public:
  void begin(int speed) {}
  void println(const char* s) { printf("%s\n", s); }
};
static SerialMock Serial;