#pragma once
class Imu {
 public:
  struct Sample {
    float gx, gy, gz;
  };
  inline static Sample sample{};
  inline static bool readable = true;
  bool begin() { return true; }
  bool hasGyroscope() const { return true; }
  bool sleep() { return true; }
  bool wake() { return true; }
  bool read(Sample& out) {
    out = sample;
    return readable;
  }
};
