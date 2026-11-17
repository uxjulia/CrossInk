"""Exercise the production USB sleep overlay without a panel or firmware build."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/main.cpp").read_text()
start = source.index("struct SleepChargeOverlay {")
end = source.index("\n#endif", start)
production = source[start:end]
harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>
constexpr int SMALL_FONT_ID = 1;
namespace HalDisplay { constexpr int FAST_REFRESH = 1; }
int monoPaints = 0;
bool allocationFails = false, locked = false, usb = true, wake = false;
bool retainedSupported = true, restoreFails = false;
int logs = 0, callbacks = 0, paints = 0, restores = 0, lockAcquisitions = 0;
bool callerOwnsLock = false;
int bolts = 0;
#define LOG_ERR(...) ++logs
using HeapByteBuffer = std::unique_ptr<uint8_t[]>;
HeapByteBuffer makePsramByteBufferNoThrow(size_t bytes) {
  return allocationFails ? nullptr : std::make_unique<uint8_t[]>(bytes);
}
struct Rect { int x, y, width, height; };
struct RenderLock {
  static bool heldByCurrentTask() { return locked; }
  RenderLock() { assert(!locked); locked = true; ++lockAcquisitions; }
  ~RenderLock() { locked = false; }
};
struct Renderer {
  std::array<uint8_t, 100> frame;
  uint8_t* getFrameBuffer() { return frame.data(); }
  size_t getBufferSize() { return frame.size(); }
  int getScreenWidth() { return 100; }
  int getScreenHeight() { return 200; }
  int getLineHeight(int) { return 12; }
  int getTextWidth(int, const char*) { return 18; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) { *t=3; *r=5; *b=7; *l=9; }
  void fillRect(int x, int y, int w, int h, bool) {
    assert(locked);
    if (x == 9) assert(y == 165 && w == 86 && h == 28);
    frame[0] = 0;
  }
  void displayBuffer(int) { assert(locked); ++monoPaints; }
  void drawText(int, int, int, const char*) { assert(locked); }
  bool displayRetainedGrayscaleOverlay(int x, int y, int w, int h, bool restore = false) {
    assert(locked && x == 9 && y == 165 && w == 86 && h == 28);
    if (!retainedSupported) return false;
    if (restore) { ++restores; return !restoreFails; }
    ++paints;
    return true;
  }
} renderer;
struct Metrics { int batteryWidth = 20, batteryHeight = 10; };
struct UITheme {
  static UITheme& getInstance() { static UITheme theme; return theme; }
  Metrics getMetrics() { return {}; }
};
struct BaseTheme {
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(Renderer&, int, int, int, int) { assert(locked); }
  static void drawBatteryLightningBolt(Renderer&, int, int, bool black) { assert(locked && black); ++bolts; }
};
struct Theme {
  void drawBatteryLeft(Renderer& r, Rect, uint16_t) { assert(locked); r.frame[1] = 0; }
} GUI;
struct GPIO { bool isUsbConnected() { return usb; } } gpio;
struct PowerManager {
  bool holdWhileUsbPowered(GPIO&, void (*callback)(void*, bool, uint16_t) = nullptr, void* context = nullptr) {
    assert(locked == callerOwnsLock);
    if (callback) {
      for (auto percent : {42, 43}) {
        ++callbacks;
        callback(context, percent == 42, percent);
        assert(std::all_of(renderer.frame.begin(), renderer.frame.end(), [](auto b) { return b == 0xa5; }));
      }
    }
    return wake;
  }
} powerManager;
'''
checks = r'''
void reset() {
  renderer.frame.fill(0xa5);
  allocationFails = locked = wake = restoreFails = false;
  usb = retainedSupported = true;
  logs = callbacks = paints = restores = lockAcquisitions = 0;
  callerOwnsLock = false; bolts = monoPaints = 0;
}
int main() {
  reset();
  assert(!holdSleepWhileUsbPowered());
  assert(callbacks == 2 && paints == 2 && restores == 1 && logs == 0 && !locked);
  assert(lockAcquisitions == 3 && bolts == 1);
  // Sleep can be entered while ActivityManager already owns its non-recursive lock.
  // Both painting and restoring must preserve that ownership without reacquiring.
  reset(); callerOwnsLock = true;
  {
    RenderLock callerLock;
    assert(!holdSleepWhileUsbPowered());
    assert(locked && lockAcquisitions == 1 && paints == 2 && restores == 1);
  }
  assert(!locked);
  reset(); callerOwnsLock = true; wake = true;
  {
    RenderLock callerLock;
    assert(holdSleepWhileUsbPowered());
    assert(locked && lockAcquisitions == 1 && paints == 2 && restores == 0);
  }
  assert(!locked);
  reset(); wake = true;
  assert(holdSleepWhileUsbPowered());
  assert(paints == 2 && restores == 0);
  reset(); retainedSupported = false;
  assert(!holdSleepWhileUsbPowered());
  assert(paints == 0 && restores == 0 && !locked);
  reset(); allocationFails = true;
  assert(!holdSleepWhileUsbPowered());
  assert(callbacks == 0 && paints == 0 && logs == 1);
  reset(); usb = false;
  assert(!holdSleepWhileUsbPowered());
  assert(callbacks == 0 && logs == 0);
  reset(); retainedSupported = false;
  assert(!holdSleepWhileUsbPowered(true));
  assert(monoPaints == 3 && paints == 0 && restores == 0);
  reset(); wake = true;
  assert(holdSleepWhileUsbPowered(true));
  assert(monoPaints == 2);
  reset(); restoreFails = true;
  assert(!holdSleepWhileUsbPowered());
  assert(restores == 1 && logs == 1);
}
'''
with tempfile.TemporaryDirectory(prefix="sleep-charge-overlay-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    cpp.write_text(harness + production + checks)
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: USB sleep overlay bounds, image preservation, unplug/wake, unsupported driver and OOM")

power_source = (ROOT / "lib/hal/HalPowerManager.cpp").read_text()
start = power_source.index("bool HalPowerManager::holdWhileUsbPowered(")
end = power_source.index("\n}\n#endif", start) + 2
power_harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
using gpio_num_t = int;
constexpr int ESP_OK = 0, LOW = 0, HIGH = 1;
constexpr int GPIO_INTR_LOW_LEVEL = 1, GPIO_INTR_DISABLE = 0, ESP_SLEEP_WAKEUP_ALL = 0;
unsigned long tick = 0;
int batteryReads = 0, timerSleeps = 0;
bool connected = true, interruptMaskWorks = true, wakeByButton = false;
std::vector<uint16_t> percentages;
std::vector<bool> charges;
bool changeCharging = false, statusKnown = true, resumeCharging = false;
struct BatteryMonitor {
  struct Status { bool chargingKnown, charging; };
  Status readStatus() const { return {statusKnown, resumeCharging ? tick >= 30000 : (!changeCharging || tick < 30000)}; }
};
std::vector<unsigned long> callbackTimes;
#define LOG_INF(...) do {} while (0)
#define LOG_ERR(...) do {} while (0)
namespace BoardConfig { struct { struct { int power = 3; } input; } ACTIVE; }
struct HalGPIO { bool isUsbConnected() { return connected; } };
void disableWiFiBeforeDeepSleep() {}
int gpio_intr_disable(int pin) { assert(pin == 3); return interruptMaskWorks ? ESP_OK : -1; }
int digitalRead(int) { return wakeByButton && tick >= 30000 ? LOW : HIGH; }
unsigned long millis() { return tick; }
void delay(int ms) { tick += ms; if (tick >= 90000) connected = false; }
void esp_sleep_enable_timer_wakeup(uint64_t us) { assert(us == 30000000); }
void gpio_wakeup_enable(int, int) {}
void esp_sleep_enable_gpio_wakeup() {}
void esp_light_sleep_start() { ++timerSleeps; delay(30000); }
void gpio_wakeup_disable(int) {}
void gpio_set_intr_type(int, int type) { assert(type == GPIO_INTR_DISABLE); }
void esp_sleep_disable_wakeup_source(int) {}
struct HalPowerManager {
  static constexpr unsigned long BATTERY_POLL_MS = 30000;
  uint16_t getBatteryPercentage() const { ++batteryReads; return 42 + tick / 60000; }
  bool holdWhileUsbPowered(HalGPIO&, void (*)(void*, bool, uint16_t) = nullptr, void* = nullptr) const;
};
void status(void* context, bool charging, uint16_t percent) {
  assert(context == &percentages);
  percentages.push_back(percent);
  charges.push_back(charging);
  callbackTimes.push_back(tick);
}
'''
power_checks = r'''
void reset() {
  tick = 0; batteryReads = timerSleeps = 0;
  connected = interruptMaskWorks = true; wakeByButton = false;
  percentages.clear(); callbackTimes.clear(); charges.clear();
  changeCharging = resumeCharging = false; statusKnown = true;
}
int main() {
  HalGPIO gpio;
  HalPowerManager power;
  for (bool lightSleep : {true, false}) {
    reset(); interruptMaskWorks = lightSleep;
    assert(!power.holdWhileUsbPowered(gpio, status, &percentages));
    assert((percentages == std::vector<uint16_t>{42, 43}));
    assert((callbackTimes == std::vector<unsigned long>{0, 60000}));
    assert(batteryReads == 3 && timerSleeps == (lightSleep ? 3 : 0));
  }
  for (bool lightSleep : {true, false}) {
    reset(); interruptMaskWorks = lightSleep; changeCharging = true;
    assert(!power.holdWhileUsbPowered(gpio, status, &percentages));
    assert((percentages == std::vector<uint16_t>{42, 42, 43}));
    assert((charges == std::vector<bool>{true, false, false}));
    assert((callbackTimes == std::vector<unsigned long>{0, 30000, 60000}));
  }
  reset(); resumeCharging = true;
  assert(!power.holdWhileUsbPowered(gpio, status, &percentages));
  assert((percentages == std::vector<uint16_t>{42, 42, 43}));
  assert((charges == std::vector<bool>{false, true, true}));
  reset(); statusKnown = false;
  assert(!power.holdWhileUsbPowered(gpio, status, &percentages));
  assert((charges == std::vector<bool>{false, false}));
  reset(); wakeByButton = true;
  assert(power.holdWhileUsbPowered(gpio, status, &percentages));
  assert(timerSleeps == 1 && percentages.size() == 1);
  reset(); assert(!power.holdWhileUsbPowered(gpio));
  assert(batteryReads == 0);
  reset(); connected = false;
  assert(!power.holdWhileUsbPowered(gpio, status, &percentages));
  assert(batteryReads == 0 && percentages.empty());
}
'''
with tempfile.TemporaryDirectory(prefix="sleep-charge-poll-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    cpp.write_text(power_harness + power_source[start:end] + power_checks)
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: production USB polling callback cadence, change-only paints, interrupt fallback and wake")
