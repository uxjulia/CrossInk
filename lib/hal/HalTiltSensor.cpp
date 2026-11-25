#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  gx = sample.gx;
  gy = sample.gy;
  gz = sample.gz;
  return true;
}

void HalTiltSensor::begin() {
#ifdef FORCE_TILT_SENSOR_AVAILABLE
  _available = true;
  _isAwake = false;
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "Tilt sensor override active via build flag");
  return;
#endif

  _available = false;
  if (!_sdkImu.begin()) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }
  // Stand by even accelerometer-only parts; they cannot drive gyro page turns.
  if (!_sdkImu.sleep()) LOG_ERR("GYR", "IMU standby failed");
  _available = _sdkImu.hasGyroscope();
  if (!_available) {
    LOG_INF("GYR", "IMU has no gyroscope");
    return;
  }
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "SDK IMU initialized");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const uint8_t enabled, const uint8_t direction, const uint8_t orientation,
                           const bool inReader, const uint8_t sensitivity) {
  if (!_available) {
    return;
  }

  const bool shouldBeAwake = (enabled != CrossPointTiltPageTurn::TILT_OFF) && inReader;

  // State machine: keep the sensor awake only while tilt is enabled in a reader.
  if (shouldBeAwake && !_isAwake) {
    _isAwake = wake();
    return;
  } else if (!shouldBeAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled or outside a reader, skip polling and avoid unnecessary I2C traffic.
  if (!shouldBeAwake) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) {
    return;
  }

  // Map the gyro axis to the selected gesture based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  const bool forwardBack = direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK ||
                           direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK_INVERTED;
  const bool inverted = direction == CrossPointTiltPageTurnDirection::TILT_LEFT_RIGHT ||
                        direction == CrossPointTiltPageTurnDirection::TILT_FORWARD_BACK_INVERTED;
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = forwardBack ? gy : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = forwardBack ? -gy : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = forwardBack ? gx : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = forwardBack ? -gx : gy;
      break;
    default:
      tiltAxis = forwardBack ? gy : gx;
      break;
  }
  if (inverted) {
    tiltAxis = -tiltAxis;
  }

  // Keep Normal's existing board calibration. Low stays below the Sticky's
  // ±245 dps range; High accepts gentler flicks without changing rearm/cooldown.
  float rateThreshold = RATE_THRESHOLD_DPS;
  if (sensitivity == TiltSensitivity::LOW_SENSITIVITY) rateThreshold *= 1.15f;
  if (sensitivity == TiltSensitivity::HIGH_SENSITIVITY) rateThreshold *= 0.75f;

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > rateThreshold) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -rateThreshold) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
