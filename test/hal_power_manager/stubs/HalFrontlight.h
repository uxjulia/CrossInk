#pragma once
#include <Arduino.h>
struct TestFrontlight {
  void setOn(bool on) { power_test::lightOn = on; }
};
inline TestFrontlight Frontlight;
