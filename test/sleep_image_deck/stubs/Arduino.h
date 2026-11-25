#pragma once
#include <cassert>
#include <random>
inline std::mt19937 sleepRandom(12345);
inline long forcedSleepRank = -1;
inline long random(long count) {
  if (forcedSleepRank >= 0) {
    assert(forcedSleepRank < count);
    return forcedSleepRank;
  }
  return std::uniform_int_distribution<long>(0, count - 1)(sleepRandom);
}
inline void delay(unsigned) {}
