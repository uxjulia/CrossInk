#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "HalTiltSensor.h"
#include "ReaderTiltPageTurn.h"

unsigned long testNow = 0;
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
constexpr float NORMAL_THRESHOLD = 200.0f;
#else
constexpr float NORMAL_THRESHOLD = 270.0f;
#endif

void start(HalTiltSensor& sensor, uint8_t sensitivity) {
  testNow = 0;
  Imu::sample = {};
  Imu::readable = true;
  sensor.begin();
  sensor.update(1, 1, 0, true, sensitivity);
}

ReaderTiltPageTurn poll(HalTiltSensor& sensor, unsigned long now, float rate, uint8_t sensitivity,
                        bool nextOnly = false, uint8_t direction = 1, uint8_t orientation = 0) {
  testNow = now;
  Imu::sample = {rate, rate, 0};
  sensor.update(1, direction, orientation, true, sensitivity);
  return detectReaderTiltPageTurn(sensor, true, nextOnly);
}

bool triggers(float rate, uint8_t sensitivity) {
  HalTiltSensor sensor;
  start(sensor, sensitivity);
  const auto turn = poll(sensor, 1000, rate, sensitivity);
  return turn.prev || turn.next;
}

int main() {
  // Normal, missing and invalid preferences retain the original strict boundary.
  for (const uint8_t level : {uint8_t(TiltSensitivity::NORMAL), uint8_t(255)}) {
    assert(!triggers(NORMAL_THRESHOLD, level));
    assert(triggers(NORMAL_THRESHOLD + 1, level));
    assert(!triggers(-NORMAL_THRESHOLD, level));
    assert(triggers(-NORMAL_THRESHOLD - 1, level));
  }
  {
    HalTiltSensor sensor;
    start(sensor, TiltSensitivity::NORMAL);
    testNow = 1000;
    Imu::sample = {NORMAL_THRESHOLD, 0, 0};
    sensor.update(1, 1, 0, true);  // Existing four-argument callers keep Normal.
    assert(!sensor.wasTiltedForward());
    testNow = 1050;
    Imu::sample.gx += 1;
    sensor.update(1, 1, 0, true);
    assert(sensor.wasTiltedForward());
  }
  assert(triggers(NORMAL_THRESHOLD - 20, TiltSensitivity::HIGH));
  assert(!triggers(NORMAL_THRESHOLD - 20, TiltSensitivity::NORMAL));
  assert(!triggers(NORMAL_THRESHOLD + 10, TiltSensitivity::LOW));
  assert(triggers(NORMAL_THRESHOLD * 1.2f, TiltSensitivity::LOW));
  for (const uint8_t level : {TiltSensitivity::LOW, TiltSensitivity::NORMAL, TiltSensitivity::HIGH}) {
    const float strong = NORMAL_THRESHOLD * 1.2f;
    // Presets must remain reachable within the physical gyro range.
    assert(strong < (NORMAL_THRESHOLD == 200 ? 245.0f : 512.0f));
    for (const float sign : {-1.0f, 1.0f}) {
      HalTiltSensor sensor;
      start(sensor, level);
      auto turn = poll(sensor, 1000, sign * strong, level);
      assert(turn.next == (sign > 0) && turn.prev == (sign < 0));
      turn = poll(sensor, 1700, sign * strong, level);
      assert(!turn.prev && !turn.next);  // Held motion still requires neutral.
      poll(sensor, 1750, 0, level);
      turn = poll(sensor, 1800, -sign * strong, level, true);
      assert(turn.next && !turn.prev);  // Either direction works in Next Only.
    }
    {
      HalTiltSensor sensor;
      start(sensor, level);
      assert(poll(sensor, 1000, strong, level).next);
      poll(sensor, 1050, 0, level);
      assert(!poll(sensor, 1500, strong, level).next);
      assert(poll(sensor, 1600, strong, level).next);  // Same 600 ms cooldown.
    }
    for (uint8_t orientation = 0; orientation < 4; ++orientation) {
      for (uint8_t direction = 0; direction < 4; ++direction) {
        HalTiltSensor sensor;
        start(sensor, level);
        auto turn = poll(sensor, 1000, strong, level, true, direction, orientation);
        assert(turn.next && !turn.prev);
      }
    }
    {
      HalTiltSensor sensor;
      start(sensor, level);
      Imu::readable = false;
      assert(!poll(sensor, 1000, strong, level).next);
      Imu::readable = true;
      assert(poll(sensor, 1050, strong, level).next);
    }
  }
  std::puts(
      "PASS: sensitivity, Normal compatibility, invalid fallback, directions/orientations, Next Only, rearm and "
      "cooldown");
}
