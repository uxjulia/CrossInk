#pragma once

#include <utility>

class HalTiltSensor {
 public:
  bool forward = false;
  bool backward = false;
  int forwardReads = 0;
  int backwardReads = 0;

  bool wasTiltedForward() {
    ++forwardReads;
    return std::exchange(forward, false);
  }
  bool wasTiltedBack() {
    ++backwardReads;
    return std::exchange(backward, false);
  }
};
