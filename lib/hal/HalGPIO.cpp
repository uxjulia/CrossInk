#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

#include <algorithm>

#include "InputWake.h"

#if FREEINK_MCU_S3
#include <soc/usb_serial_jtag_reg.h>
#endif

#if FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
extern "C" bool tud_mounted(void);
#endif

// Global HalGPIO instance
HalGPIO gpio;

namespace {
constexpr unsigned long BUTTON_DEBOUNCE_REPOLL_MS = 6;
// Poll cadence adapted from Sichroteph/YACP commit
// 6d1f10f4bae52d282a088f9b2e45aaac96da8377 (MIT).
constexpr unsigned long X3_USB_POLL_MS = 1000;

// The X3-vs-X4 fingerprint (freeink::detectXteinkVerdict) only makes sense on
// Xteink hardware; other boards keep their compile-time BoardConfig profile.
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run the SDK's X3 fingerprint probe and persist the result.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "X3 probe scores: pass1=%u pass2=%u", score1, score2);

  switch (verdict) {
    case freeink::XteinkVerdict::X3Confirmed:
      writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
      return HalGPIO::DeviceType::X3;
    case freeink::XteinkVerdict::X4Confirmed:
      writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
      return HalGPIO::DeviceType::X4;
    case freeink::XteinkVerdict::Inconclusive:
      break;
  }

  // Conservative fallback for first boot with inconclusive probes; not cached,
  // so the next boot re-probes.
  return HalGPIO::DeviceType::X4;
}

// --- X3 panel-controller fingerprint (UC8253 vs UC8279) ----------------------
// Newer X3 production units ship a UC8279d panel controller on the same board,
// glass and pins. The SDK probe reads the UC8279's VER/FLG registers over a
// bit-banged half-duplex SPI on the EPD pins; same override/cache scheme as
// the device fingerprint (values reuse NvsDeviceValue: 1 = UC8253, 2 = UC8279).
constexpr char NVS_KEY_EPD_OVERRIDE[] = "epd_ovr";  // 0=auto, 1=uc8253, 2=uc8279
constexpr char NVS_KEY_EPD_CACHED[] = "epd_det";    // 0=unknown, 1=uc8253, 2=uc8279

bool detectX3DisplayIsUc8279() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_EPD_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue != NvsDeviceValue::Unknown) {
    LOG_INF("HW", "EPD controller override active: %s", overrideValue == NvsDeviceValue::X3 ? "UC8279" : "UC8253");
    return overrideValue == NvsDeviceValue::X3;
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue != NvsDeviceValue::Unknown) {
    LOG_INF("HW", "Using cached EPD controller: %s", cachedValue == NvsDeviceValue::X3 ? "UC8279" : "UC8253");
    return cachedValue == NvsDeviceValue::X3;
  }

  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  const freeink::X3DisplayVerdict verdict = freeink::detectX3DisplayController(ver, &flg);
  LOG_INF("HW", "EPD probe: ver=%02X %02X %02X %02X %02X flg=%02X verdict=%u", ver[0], ver[1], ver[2], ver[3], ver[4],
          flg, static_cast<unsigned>(verdict));
  if (verdict == freeink::X3DisplayVerdict::Uc8279Confirmed) {
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X3);
    return true;
  }
  if (verdict == freeink::X3DisplayVerdict::Uc8253Assumed) {
    writeNvsDeviceValue(NVS_KEY_EPD_CACHED, NvsDeviceValue::X4);
  }
  // Inconclusive: run as UC8253 (the shipping controller) but don't persist,
  // so a flaky first boot gets re-probed.
  return false;
}

#endif  // FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3

}  // namespace

void HalGPIO::begin() {
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
#ifdef FORCE_DEVICE_X3
  _deviceType = DeviceType::X3;
  LOG_INF("HW", "Device override active via build flag: X3");
#else
  _deviceType = detectDeviceTypeWithFingerprint();
#endif
  // The panel-controller probe bit-bangs the EPD pins, so it must run before
  // SPI.begin() attaches them to the SPI matrix. I2C-only device fingerprint
  // above doesn't care about SPI ordering.
  const bool x3IsUc8279 = deviceIsX3() && detectX3DisplayIsUc8279();
  BoardConfig::selectDevice(!deviceIsX3() ? BoardConfig::Board::XteinkX4
                            : x3IsUc8279  ? BoardConfig::Board::XteinkX3Uc8279
                                          : BoardConfig::Board::XteinkX3);

  // CrossInk's X3 override/cache remains authoritative. X4 has no equivalent
  // local override, so use the SDK's factory-aware controller selection before
  // SPI claims the display pins.
  if (deviceIsX4()) {
    freeink::applyXteinkDisplayController();
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#endif
  inputMgr.begin();
  InputWake::begin();
}

bool HalGPIO::waitForInput(const uint32_t timeoutMs) { return InputWake::wait(timeoutMs, inputMgr); }
void HalGPIO::startButtonCapture() { InputWake::startCapture(); }
void HalGPIO::stopButtonCapture() { InputWake::stopCapture(); }
uint32_t HalGPIO::buttonSampleTime() const { return InputWake::capturing() ? buttonFrame.timestamp : millis(); }

bool HalGPIO::hasPendingButtonInput() const { return InputWake::hasPendingButtons() || inputMgr.isDebouncePending(); }

bool HalGPIO::prepareForDeepSleep() { return inputMgr.prepareForDeepSleep(); }

bool HalGPIO::hasToggleSwitch() const { return inputMgr.hasToggleSwitch(); }

bool HalGPIO::isToggleSwitchOn() const { return inputMgr.isToggleSwitchOn(); }

void HalGPIO::update() {
  if (InputWake::capturing()) {
#if CROSSINK_APP_CAP_TOUCH
    // Touch remains owned by the main thread. Physical buttons come from the
    // independent sampler; on C3 this also avoids a second ADC reader.
    inputMgr.update();
#endif
    buttonFrame = InputWake::takeFrame();
    if (buttonFrame.pressed || buttonFrame.released) {
      LOG_DBG("INPUT", "Buttons @%lu: state=%02x press=%02x release=%02x held=%lu",
              static_cast<unsigned long>(buttonFrame.timestamp), buttonFrame.state, buttonFrame.pressed,
              buttonFrame.released, static_cast<unsigned long>(buttonFrame.heldMs));
    }
  } else {
    inputMgr.update();
    if (inputMgr.isDebouncePending()) {
      // The SDK commits a state change after it remains stable for more than
      // 5 ms. Re-poll before the low-power loop's next ~100 ms sample so short
      // physical button presses are not discarded.
      delay(BUTTON_DEBOUNCE_REPOLL_MS);
      inputMgr.update();
    }
  }

  usbStateChanged = false;
  const unsigned long now = millis();
  if (deviceIsX3() && usbStateSampled && now - lastUsbPollMs < X3_USB_POLL_MS) {
    return;
  }

  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
  lastUsbPollMs = now;
  usbStateSampled = true;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  return InputWake::capturing() ? buttonFrame.state & (1u << buttonIndex) : inputMgr.isPressed(buttonIndex);
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  return InputWake::capturing() ? buttonFrame.pressed & (1u << buttonIndex) : inputMgr.wasPressed(buttonIndex);
}

bool HalGPIO::wasAnyPressed() const {
  return InputWake::capturing() ? buttonFrame.pressed != 0 : inputMgr.wasAnyPressed();
}

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  return InputWake::capturing() ? buttonFrame.released & (1u << buttonIndex) : inputMgr.wasReleased(buttonIndex);
}

bool HalGPIO::wasAnyReleased() const {
  return InputWake::capturing() ? buttonFrame.released != 0 : inputMgr.wasAnyReleased();
}

unsigned long HalGPIO::getHeldTime() const {
  return InputWake::capturing() ? buttonFrame.heldMs : inputMgr.getHeldTime();
}

unsigned long HalGPIO::getPowerButtonHeldTime() const {
  return InputWake::capturing() ? buttonFrame.powerHeldMs : inputMgr.getPowerButtonHeldTime();
}

#if CROSSINK_APP_CAP_TOUCH
bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::supportsMultiTouch() const { return inputMgr.supportsMultiTouch(); }

HalGPIO::TouchSnapshot HalGPIO::getTouchSnapshot() const {
  TouchSnapshot result;
  if (!supportsMultiTouch()) return result;

  const auto source = inputMgr.getTouchSnapshot();
  const auto& touch = BoardConfig::ACTIVE.touch;
  const uint16_t width = touch.rawMaxX > touch.rawMinX ? touch.rawMaxX - touch.rawMinX : 1;
  const uint16_t height = touch.rawMaxY > touch.rawMinY ? touch.rawMaxY - touch.rawMinY : 1;
  result.count = std::min<uint8_t>(source.count, TouchSnapshot::MAX_CONTACTS);
  result.reportedCount = source.reportedCount;
  for (uint8_t i = 0; i < result.count; ++i) {
    const auto& point = source.points[i];
    result.contacts[i].id = point.id;
    result.contacts[i].nx = std::clamp(static_cast<float>(point.point.x) / width, 0.0f, 1.0f);
    result.contacts[i].ny = std::clamp(static_cast<float>(point.point.y) / height, 0.0f, 1.0f);
  }
  return result;
}

bool HalGPIO::wasCompletedMultiTouchSwipe(CompletedMultiTouchSwipe& swipe) const {
  if (!supportsMultiTouch()) return false;
  return inputMgr.wasMultiTouchSwipe(swipe.contactCount, swipe.nxStart, swipe.nyStart, swipe.nxEnd, swipe.nyEnd,
                                     swipe.durationMs);
}

bool HalGPIO::wasCompletedMultiTouchRotation(CompletedMultiTouchRotation& rotation) const {
  if (!supportsMultiTouch()) return false;
  return inputMgr.wasMultiTouchRotation(rotation.degrees, rotation.nxCenter, rotation.nyCenter, rotation.durationMs);
}

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyPressed() const { return inputMgr.wasHomeKeyPressed(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }
#endif

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputWake::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::verifyPowerButtonWakeup(const bool shortPressWakes, const uint16_t longHoldMs) {
  // M5Paper v1.1 reaches setup after a normal wheel click has already been
  // released. Its hardware pull-ups make this ghost-wake debounce unnecessary.
  if (BoardConfig::isPaperMono() || BoardConfig::isM5PaperV11() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }

  constexpr unsigned long POWER_WAKE_STABILITY_MS = 10;
  const bool heldAtFirstSample = inputMgr.isPowerButtonPhysicallyPressed();
  const unsigned long sampleStart = millis();
  inputMgr.update();
  while (millis() - sampleStart < POWER_WAKE_STABILITY_MS || inputMgr.isDebouncePending()) {
    delay(1);
    inputMgr.update();
  }
  if (shortPressWakes) return true;
  if (!heldAtFirstSample || !inputMgr.isPowerButtonPhysicallyPressed()) return false;

  // Deep sleep wakes as soon as the GPIO changes. Keep the panel and SD card
  // asleep until the held press qualifies as a long Power gesture. millis()
  // starts at reset, so time spent reaching this early boot check counts too.
  while (millis() < longHoldMs) {
    delay(1);
    inputMgr.update();
    if (!inputMgr.isPowerButtonPhysicallyPressed()) return false;
  }
  return true;
}

#if FREEINK_MCU_S3
// USB host presence via the USB-Serial-JTAG peripheral's SOF frame counter: a
// connected host clocks 1 kHz start-of-frame packets, so the counter advancing
// between polls means a data-capable host is attached. Held for a short window
// so multiple callers within one loop tick (update() plus header renders) all
// see the same answer. Limitation: a data-less wall charger sends no SOFs and
// stays invisible — on boards with no VBUS line (X4 Pro, see
// xteink-x4pro-support.md) this is the only observable USB signal.
static bool usbHostSofActive() {
  static uint32_t lastFrame = 0;
  static unsigned long lastAdvanceMs = 0;
  static bool seeded = false;
  if (!seeded) {
    // First probe must not fabricate a connection: getWakeupReason() calls this
    // at boot, and a false positive turns a power-button wake (POWERON reset)
    // into AfterUSBPower, which goes straight back to deep sleep — the device
    // never wakes. Seed the counter and wait one SOF period out; a real host
    // clocks SOFs at 1 kHz, so 3 ms guarantees advancement when attached.
    seeded = true;
    lastFrame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
    delay(3);
  }
  const uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
  if (frame != lastFrame) {
    lastFrame = frame;
    lastAdvanceMs = millis();
    return true;
  }
  return lastAdvanceMs != 0 && millis() - lastAdvanceMs < 1500;
}
#endif

bool HalGPIO::isUsbConnected() const {
#if FREEINK_DEVICE_X3
  if (deviceIsX3()) {
    // X3 has no USB-detect pin; infer external power from the gauge's charge
    // current via the SDK's BatteryMonitor (BQ27220 Current() > 0 = charging).
    static const BatteryMonitor battery;
    return battery.isCharging();
  }
#endif
#if FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
  // X4 Pro uses native TinyUSB for its composite CDC+MSC device. The mounted
  // state is the reliable bus-presence signal for this OTG configuration.
  if (tud_mounted()) return true;
#endif
  if (BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
#if FREEINK_MCU_S3
#if !FREEINK_DEVICE_PICCO
  // Native-USB host traffic is cheap to sample; charging is the fallback for
  // power-only adapters on boards without input-rail telemetry.
  if (usbHostSofActive()) return true;
#endif
  static const BatteryMonitor battery;
  static bool sampled = false;
  static bool powerPresent = false;
  static unsigned long lastSampleMs = 0;
  const unsigned long now = millis();
  if (!sampled || now - lastSampleMs >= 500) {
#if FREEINK_DEVICE_PICCO
    // Rate-limit shared-I2C charger reads, including when the battery is full.
    bool known = false;
    powerPresent = battery.isExternalPowerPresent(&known);
    if (!known) powerPresent = usbHostSofActive() || battery.isCharging();
#else
    powerPresent = battery.isCharging();
#endif
    lastSampleMs = now;
    sampled = true;
  }
  return powerPresent;
#else
  return false;
#endif
}

bool HalGPIO::isUsbConnectedCached() const { return usbStateSampled ? lastUsbConnected : isUsbConnected(); }

bool HalGPIO::coldBootImpliesPowerButton() const {
  // These boards use a button-energized or otherwise known latch topology, so
  // a no-USB POWERON can be trusted as a power-button boot. Unknown/future
  // topologies must continue booting instead of risking an immediate sleep
  // after flashing or when charge detection is unavailable.
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky();
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected &&
      coldBootImpliesPowerButton()) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
