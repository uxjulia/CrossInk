#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#include "HalGPIO.h"

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
#include <HalFrontlight.h>
#endif

#if FREEINK_DEVICE_PICCO
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>

#include "BatteryEstimateRetention.h"

namespace {
RTC_NOINIT_ATTR BatteryEstimateRetention retainedBattery;

// Main-loop charging polls and render-task status bars share this estimate.
// Reuse the already-created power mutex; no extra heap allocation or task.
class BatterySampleLock {
 public:
  explicit BatterySampleLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~BatterySampleLock() {
    if (mutex_) xSemaphoreGive(mutex_);
  }
  BatterySampleLock(const BatterySampleLock&) = delete;
  BatterySampleLock& operator=(const BatterySampleLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};
}  // namespace
#endif

HalPowerManager powerManager;  // Singleton instance

namespace {
void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(true);
  }
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}
}  // namespace

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
#if FREEINK_DEVICE_PICCO
  if (BatteryMonitor::configureCharger()) {
    LOG_INF("PWR", "Charger configured");
  } else {
    LOG_ERR("PWR", "Charger configuration failed");
  }
#endif
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

bool HalPowerManager::updateBatteryCalibration() {
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX3 &&
      BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX3Uc8279)
    return false;

  // The SDK performs one best-effort attempt per boot. False is terminal,
  // including I2C failure; an interrupted/failed load is retried next boot.
  const bool wasPending = batteryCalibrationPending;
  batteryCalibrationPending = BatteryMonitor::loadDesignCapacity();
  if (wasPending && !batteryCalibrationPending) {
    _batteryPollStarted = false;  // Sample the corrected gauge on the next battery read.
    LOG_INF("PWR", "X3 battery capacity check finished");
  }
  return batteryCalibrationPending;
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  if (modeMutex != nullptr) {
    xSemaphoreTake(modeMutex, portMAX_DELAY);
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  const bool locked = lockCount > 0;

  if (!locked && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      if (modeMutex != nullptr) {
        xSemaphoreGive(modeMutex);
      }
      return;
    }
    isLowPower = true;

  } else if ((!enabled || locked) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      if (modeMutex != nullptr) {
        xSemaphoreGive(modeMutex);
      }
      return;
    }
    isLowPower = false;
  }

  if (modeMutex != nullptr) {
    xSemaphoreGive(modeMutex);
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
  // The light and touch share I2C. Turn the light off before touch releases it.
  Frontlight.setOn(false);
  if (!gpio.prepareForDeepSleep()) LOG_ERR("PWR", "Touch sleep preparation failed");
#endif
  // Once started, let the SDK exit configuration mode and seal the gauge.
  // Sleep runs on the main task after rendering has stopped. The SDK bounds
  // every wait; ordinary sleep has no delay once the startup check is done.
  while (batteryCalibrationPending && BatteryMonitor::loadDesignCapacity()) {
    delay(20);
  }
  disableWiFiBeforeDeepSleep();

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  // Release every configured battery latch. BoardConfig owns the pin mapping;
  // the collision guard prevents a stale/mismatched profile from driving a
  // display or SD bus pin low and holding it through sleep.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || BoardConfig::latchConflictsWithBus(pin)) continue;
    const auto latch = static_cast<gpio_num_t>(pin);
    gpio_set_direction(latch, GPIO_MODE_OUTPUT);
    gpio_set_level(latch, 0);
    gpio_hold_en(latch);
  }
#else
  // Keep configured power latches asserted through deep sleep. The SDK isolates
  // GPIO pads before sleeping, so an unheld latch can float LOW once external
  // power is removed and turn a fast wake into a cold boot. This is deliberately
  // complementary to the C3 path above, where the battery latch must go LOW.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || BoardConfig::latchConflictsWithBus(pin)) continue;
    const auto latch = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(latch);
    gpio_set_direction(latch, GPIO_MODE_OUTPUT);
    gpio_set_level(latch, 1);
    gpio_hold_en(latch);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

  // The SDK convenience helper currently isolates every GPIO after arming the
  // wake source. On the ESP32-C3 that overwrites the power pin's sleep input
  // configuration, so short presses can be missed. Isolate first, then restore
  // and arm the board-configured power pin immediately before sleeping.
  freeink::PowerManager::waitForPowerButtonRelease();
  esp_sleep_config_gpio_isolate();
  freeink::PowerManager::armPowerButtonWakeup();
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}

#if FREEINK_DEVICE_PICCO
bool HalPowerManager::holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(void*, uint16_t), void* context) const {
  if (!gpio.isUsbConnected()) return false;
  disableWiFiBeforeDeepSleep();
  const auto powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // Input capture has stopped, but its GPIO handler remains installed. Wake
  // setup changes this pin from edge to level triggering; mask the normal CPU
  // interrupt first so a held wake press cannot trap us in an interrupt storm.
  // GPIO/RTC wake detection is independent of this runtime interrupt mask.
  const bool canLightSleep = gpio_intr_disable(powerPin) == ESP_OK;
  if (!canLightSleep) LOG_ERR("PWR", "Could not mask Power interrupt; using USB sleep polling");
  // Ignore the press that requested sleep. Picco Power is active-low.
  while (digitalRead(powerPin) == LOW) delay(10);
  LOG_INF("PWR", "USB power present, holding sleep screen");
  unsigned long lastStatusMs = 0;
  int lastPercent = -1;
  while (gpio.isUsbConnected()) {
    const auto now = millis();
    if (onStatus && (lastPercent < 0 || now - lastStatusMs >= BATTERY_POLL_MS)) {
      lastStatusMs = now;
      const uint16_t percent = getBatteryPercentage();
      if (percent != lastPercent) {
        lastPercent = percent;
        onStatus(context, percent);
      }
    }
    if (canLightSleep) {
      esp_sleep_enable_timer_wakeup(30ULL * 1000 * 1000);
      gpio_wakeup_enable(powerPin, GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      esp_light_sleep_start();
      gpio_wakeup_disable(powerPin);
      // Do not leave the level interrupt armed when input is initialized on wake.
      gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    } else {
      delay(50);
    }
    if (digitalRead(powerPin) == LOW) return true;
  }
  return false;
}
#endif

#if FREEINK_DEVICE_PICCO
void HalPowerManager::restoreBatteryAfterSleep(bool usbSleepWake) {
  const uint64_t nowMs = esp_rtc_get_time_us() / 1000;
  const bool sleepWake = esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
  _batteryResumed = (sleepWake || usbSleepWake) && retainedBattery.restore(_batteryEstimate, nowMs);
  if (_batteryResumed) {
    _batterySampleRtcMs = retainedBattery.sampledAtMs;
  } else {
    retainedBattery.marker = 0;
  }
}

uint16_t HalPowerManager::getPiccoBatteryPercentage() const {
  const BatterySampleLock lock(modeMutex);
  const unsigned long now = millis();
  if (_batteryPollStarted && now - _batteryLastPollMs < BATTERY_POLL_MS) return _batteryEstimate.percentage();
  _batteryLastPollMs = now;
  _batteryPollStarted = true;

  static const BatteryMonitor battery;
  const BatteryMonitor::Status sample = battery.readStatus();
  if (!sample.percentageKnown || !sample.millivoltsKnown) {
    LOG_ERR("PWR", "Battery voltage unavailable; keeping previous estimate");
    return _batteryEstimate.percentage();
  }
  const uint64_t nowRtcMs = esp_rtc_get_time_us() / 1000;
  const uint64_t elapsedMs = nowRtcMs >= _batterySampleRtcMs ? nowRtcMs - _batterySampleRtcMs : 0;
  _batteryEstimate.update(sample, static_cast<uint32_t>(std::min<uint64_t>(elapsedMs, UINT32_MAX)));
  _batterySampleRtcMs = nowRtcMs;
  // RTC RAM only: no SD/NVS writes and no additional sleep-time power domains.
  retainedBattery.save(_batteryEstimate, nowRtcMs);
  return _batteryEstimate.percentage();
}
#endif

uint16_t HalPowerManager::getBatteryPercentage() const {
#if FREEINK_DEVICE_PICCO
  return getPiccoBatteryPercentage();
#endif
  static const BatteryMonitor battery;
  const bool hasGauge = BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0;
  const unsigned long now = millis();
  if (_batteryPollStarted && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return hasGauge ? _batteryCachedPercent : _batteryCachedPercent / 10;
  }

  _batteryLastPollMs = now;
  if (hasGauge) {
    _batteryPollStarted = true;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // Smooth once per poll, independent of how many times the UI measures or draws it.
  const uint16_t percent = battery.readPercentage();
  if (!_batteryPollStarted) {
    _batteryCachedPercent = 10 * percent;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + percent * 10) / 10;
  }
  _batteryPollStarted = true;
  return _batteryCachedPercent / 10;
}

#if CROSSINK_BATTERY_DIAG_LOG
bool HalPowerManager::getBatteryDiagnostics(BatteryDiagnostics& out) const {
#if FREEINK_DEVICE_PICCO
  const BatterySampleLock lock(modeMutex);
#endif
  // Function-local like getBatteryPercentage()'s: BoardConfig::ACTIVE is only
  // resolved once HalGPIO::begin() has run the X3/X4 probe, so a file-scope
  // instance could be constructed against an unresolved profile.
  static const BatteryMonitor battery;
  const BatteryMonitor::Status status = battery.readStatus();
  if (!status.supported) {
    LOG_ERR("PWR", "Battery diagnostics unsupported on this board");
    return false;
  }
  out.soc = status.percentage;
  out.millivolts = status.millivolts;
  out.charging = status.charging;
  out.socKnown = status.percentageKnown;
  out.millivoltsKnown = status.millivoltsKnown;
  out.chargingKnown = status.chargingKnown;
  out.externalPower = status.externalPower;
  out.externalPowerKnown = status.externalPowerKnown;
  out.chargeComplete = status.chargeComplete;
  out.chargeCompleteKnown = status.chargeCompleteKnown;
  out.chargerStatus = status.chargerStatus;
#if FREEINK_DEVICE_PICCO
  out.displayedKnown = _batteryEstimate.known();
  out.displayedPercent = _batteryEstimate.percentage();
  out.estimateState = static_cast<int16_t>(_batteryEstimate.state().power);
  out.settlingMs = _batteryEstimate.state().settlingMs;
  const uint64_t nowMs = esp_rtc_get_time_us() / 1000;
  out.sampleAgeMs = static_cast<uint32_t>(
      std::min<uint64_t>(nowMs >= _batterySampleRtcMs ? nowMs - _batterySampleRtcMs : 0, UINT32_MAX));
  out.resumed = _batteryResumed;
#else
  out.displayedKnown = _batteryPollStarted;
  out.displayedPercent =
      BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0 ? _batteryCachedPercent : _batteryCachedPercent / 10;
  out.sampleAgeMs = millis() - _batteryLastPollMs;
#endif
  return true;
}
#endif

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  ++powerManager.lockCount;
  xSemaphoreGive(powerManager.modeMutex);
  // Immediately restore normal CPU frequency if currently in low-power mode.
  powerManager.setPowerSaving(false);
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  --powerManager.lockCount;
  xSemaphoreGive(powerManager.modeMutex);
}
