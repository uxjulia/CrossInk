#pragma once

#include <HalTiltSensor.h>

struct ReaderTiltPageTurn {
  bool prev;
  bool next;
};

inline ReaderTiltPageTurn detectReaderTiltPageTurn(HalTiltSensor& sensor, const bool enabled, const bool nextPageOnly) {
  // Both getters consume their event. Read each even when the first is true,
  // so combining directions cannot leave a second page turn pending.
  const bool forward = sensor.wasTiltedForward();
  const bool backward = sensor.wasTiltedBack();
  if (!enabled) return {false, false};
  return {backward && !nextPageOnly, forward || (backward && nextPageOnly)};
}
