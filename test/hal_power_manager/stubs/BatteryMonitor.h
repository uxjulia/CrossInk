#pragma once
#include <Arduino.h>
class BatteryMonitor {
 public:
  struct Status {
    bool supported = false;
    bool percentageKnown = false;
    bool millivoltsKnown = false;
    bool chargingKnown = false;
    bool externalPowerKnown = false;
    uint16_t percentage = 0;
    uint16_t millivolts = 0;
    bool charging = false;
    bool externalPower = false;
    bool chargeCompleteKnown = false;
    bool chargeComplete = false;
    int16_t chargerStatus = -1;  // Raw charger register, or -1 when not sampled/unknown.
    // Raw M5PM1 telemetry for diagnostics; -1 when not read (non-PM1 boards
    // or failed I/O). VIN is the DC input rail, 5VINOUT the bidirectional
    // USB-C rail, powerSource the PM1's PWR_SRC bitmap (reg 0x04 [2:0]:
    // bit0=5VIN, bit1=5VINOUT, bit2=BAT).
    int32_t pm1VinMv = -1;
    int32_t pm1VinOutMv = -1;
    int16_t pm1PowerSource = -1;
  };

  Status readStatus() const {
    ++power_test::gaugeReads;
    Status s;
    s.supported = true;
    s.percentageKnown = s.millivoltsKnown = !power_test::gaugeFails;
    s.chargingKnown = s.externalPowerKnown = s.chargeCompleteKnown = true;
    s.percentage = power_test::percentage;
    s.millivolts = power_test::batteryMv;
    s.externalPower = power_test::usbPowered;
    s.charging = power_test::charging;
    s.chargeComplete = power_test::chargeComplete;
    s.chargerStatus = power_test::chargerStatus;
    return s;
  }
  static bool configureCharger() {
    ++power_test::chargerSetups;
    return true;
  }
  static bool loadDesignCapacity() { return power_test::calibrationPending; }
  bool readPercentageChecked(uint16_t& out) const {
    ++power_test::gaugeReads;
    if (power_test::gaugeFails) return false;
    out = power_test::percentage;
    return true;
  }
  uint16_t readPercentage() const {
    ++power_test::gaugeReads;
    return power_test::percentage;
  }
};
