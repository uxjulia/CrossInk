#pragma once
#include <cstdint>
namespace frontlight_test {
inline bool detected = true;
inline bool begun = false;
inline uint8_t brightness = 0;
inline uint8_t warmth = 0;
}  // namespace frontlight_test
class FrontlightManager {
 public:
  void begin() { frontlight_test::begun = frontlight_test::detected; }
  bool present() const { return frontlight_test::begun; }
  bool hasColorTemperature() const { return present(); }
  void setBrightness(uint8_t value) { frontlight_test::brightness = value; }
  void setColorTemperature(uint8_t value) { frontlight_test::warmth = value; }
  uint8_t colorTemperature() const { return frontlight_test::warmth; }
};
