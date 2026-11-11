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
#include <vector>
constexpr int SMALL_FONT_ID = 1;
bool allocationFails = false, locked = false, usb = true, wake = false;
bool retainedSupported = true, restoreFails = false;
int logs = 0, callbacks = 0, paints = 0, restores = 0;
#define LOG_ERR(...) ++logs
using HeapByteBuffer = std::unique_ptr<uint8_t[]>;
HeapByteBuffer makePsramByteBufferNoThrow(size_t bytes) {
  return allocationFails ? nullptr : std::make_unique<uint8_t[]>(bytes);
}
struct Rect { int x, y, width, height; };
struct RenderLock {
  RenderLock() { assert(!locked); locked = true; }
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
    assert(locked && x == 9 && y == 165 && w == 86 && h == 28);
    frame[0] = 0;
  }
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
struct Theme {
  void drawBatteryLeft(Renderer& r, Rect, uint16_t) { assert(locked); r.frame[1] = 0; }
} GUI;
struct GPIO { bool isUsbConnected() { return usb; } } gpio;
struct PowerManager {
  bool holdWhileUsbPowered(GPIO&, void (*callback)(void*, uint16_t) = nullptr, void* context = nullptr) {
    assert(!locked);
    if (callback) {
      for (auto percent : {42, 43}) {
        ++callbacks;
        callback(context, percent);
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
  logs = callbacks = paints = restores = 0;
}
int main() {
  reset();
  assert(!holdSleepWhileUsbPowered());
  assert(callbacks == 2 && paints == 2 && restores == 1 && logs == 0 && !locked);
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
  bool holdWhileUsbPowered(HalGPIO&, void (*)(void*, uint16_t) = nullptr, void* = nullptr) const;
};
void status(void* context, uint16_t percent) {
  assert(context == &percentages);
  percentages.push_back(percent);
  callbackTimes.push_back(tick);
}
'''
power_checks = r'''
void reset() {
  tick = 0; batteryReads = timerSleeps = 0;
  connected = interruptMaskWorks = true; wakeByButton = false;
  percentages.clear(); callbackTimes.clear();
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
