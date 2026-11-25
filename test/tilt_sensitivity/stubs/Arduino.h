#pragma once
#include <cmath>
#include <cstdint>
// Match Arduino GPIO macros so host tests catch enum name collisions.
#define LOW 0x0
#define HIGH 0x1

extern unsigned long testNow;
inline unsigned long millis() { return testNow; }
