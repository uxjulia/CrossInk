#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

void HalFrontlight::initializeHardware() {
  if (initialized) return;
  // I2C frontlights report presence only after the SDK probes the driver.
  manager.begin();
  initialized = true;
}

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  initializeHardware();
  if (!manager.present()) {
    return;
  }
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) {
    manager.setBrightness(lastBrightness);
  }
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) {
    return;
  }
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
}
