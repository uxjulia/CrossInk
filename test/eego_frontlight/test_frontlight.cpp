#include <HalFrontlight.h>
#include <Wire.h>
#include <esp_mac.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  assert(argc == 3);
  unitUid = std::strtoul(argv[1], nullptr, 16);
  Wire.fitted = std::string(argv[2]) == "fitted";
  const bool expected = Wire.fitted && unitUid != 0xBB0408 && unitUid != 0xBB040C && unitUid != 0xBB0418;
  static_assert(FREEINK_CAP_FRONTLIGHT && FREEINK_CAP_WARMLIGHT);
  assert(!Frontlight.present());
  Frontlight.initializeHardware();
  assert(Frontlight.present() == expected);
  assert(Frontlight.hasColorTemperature() == expected);
  assert(pins[12] == LOW && Wire.writes == 0);
  Frontlight.begin(60, 50, true);
  if (!expected) {
    assert(!Frontlight.isOn() && pins[12] == LOW && Wire.writes == 0);
    return 0;
  }
  assert(Frontlight.isOn() && pins[12] == HIGH);
  // LM3630A is configured for exponential output: half the register code
  // is not half the light. A midpoint must blend equal outputs without the
  // dramatic dimming caused by the old linear-code split (76/76).
  assert(Wire.regs[3] == Wire.regs[4]);
  assert(Wire.regs[3] > 120 && Wire.regs[3] < 153);
  assert((Wire.regs[0] & 6) == 6);
  Frontlight.setWarmth(0);
  assert(Wire.regs[3] == 0 && Wire.regs[4] == 153 && (Wire.regs[0] & 6) == 2);
  Frontlight.setWarmth(100);
  assert(Wire.regs[3] == 153 && Wire.regs[4] == 0 && (Wire.regs[0] & 6) == 4);
  Frontlight.setOn(false);
  assert(pins[12] == LOW && Wire.regs[3] == 0 && Wire.regs[4] == 0);
  Frontlight.setBrightness(73);
  assert(pins[12] == LOW);
  Frontlight.setOn(true);
  assert(pins[12] == HIGH && Wire.regs[3] == 186);
  for (unsigned warmth : {0u, 50u, 100u}) {
    Frontlight.setWarmth(warmth);
    for (unsigned brightness = 1; brightness <= 100; ++brightness) {
      Frontlight.setBrightness(brightness);
      assert(pins[12] == HIGH && (Wire.regs[3] >= 4 || Wire.regs[4] >= 4));
      assert(Wire.regs[3] == 0 || Wire.regs[3] >= 4);
      assert(Wire.regs[4] == 0 || Wire.regs[4] >= 4);
    }
  }
  // A configuration failure leaves hardware disabled; next request retries.
  Frontlight.setOn(false);
  Wire.failRegister = 0x50;
  Frontlight.setOn(true);
  assert(pins[12] == LOW);
  Frontlight.setBrightness(60);
  assert(pins[12] == HIGH);
  // Production sleep switches the light off before touch releases shared I2C.
  Frontlight.setOn(false);
  assert(pins[12] == LOW);
  std::puts("EEGO fitted frontlight: detection, saved state, brightness/warmth, off/on and retry passed");
}
