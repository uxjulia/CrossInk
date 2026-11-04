#pragma once

#include <cassert>
#include <cstdint>

namespace frontlight_test {
inline bool fitted = true;
inline bool i2c = true;
inline bool warmChannel = true;
inline int begins = 0;
inline int writes = 0;
inline uint8_t brightness = 0;
inline uint8_t warmth = 0;
}  // namespace frontlight_test

class FrontlightManager {
 public:
  void begin() { ++frontlight_test::begins; }
  bool present() const { return frontlight_test::fitted && (!frontlight_test::i2c || frontlight_test::begins != 0); }
  bool hasColorTemperature() const { return present() && frontlight_test::warmChannel; }
  void setBrightness(uint8_t value) {
    assert(frontlight_test::begins == 1);
    ++frontlight_test::writes;
    frontlight_test::brightness = value;
  }
  void setColorTemperature(uint8_t value) { frontlight_test::warmth = value; }
  uint8_t colorTemperature() const { return frontlight_test::warmth; }
};
