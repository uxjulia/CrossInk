#include "StatsCounterId.h"

#ifndef SIMULATOR
#include <esp_system.h>
#else
#include <random>
#endif

StatsCounterId StatsCounterId::create() {
  StatsCounterId id;
  // Four words, no heap allocation on the device. Creation happens only when
  // starting counters, never during rendering or ordinary checkpoints.
  for (size_t i = 0; i < id.bytes.size(); i += 4) {
#ifndef SIMULATOR
    const uint32_t word = esp_random();
#else
    static std::random_device random;
    const uint32_t word = random();
#endif
    for (size_t n = 0; n < 4; ++n) id.bytes[i + n] = word >> (8 * n);
  }
  id.bytes[0] |= 1;  // Never generate the reserved legacy identity.
  return id;
}
