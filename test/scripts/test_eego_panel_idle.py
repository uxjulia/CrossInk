"""Compile the production panel-idle policy and check upstream #4494 timing."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/main.cpp').read_text()
start = source.index('static void idleDisplayWhileAwake(')
end = source.index('\nvoid loop()', start)
production = source[start:end]
loop = source[source.index('\nvoid loop()'):]
# USB work must run before its idle check, even though it skips the normal loop.
usb_start = loop.index('if (activityManager.requiresExclusiveStorageLoop())')
usb_end = loop.index('\n    return;', usb_start)
usb = loop[usb_start:usb_end]
assert usb.index('activityManager.loop();') < usb.index('idleDisplayWhileAwake(')
assert 'idleDisplayWhileAwake();' in usb
assert loop.index('mappedInputManager.update();') < usb_start
# Normal input includes tilt, sampled before the idle check and shortcut returns.
normal = loop[usb_end:]
assert normal.index('halTiltSensor.update(') < normal.index('const bool userInputReceived')
input_start = normal.index('const bool userInputReceived')
input_end = normal.index(';', input_start)
for event in ('gpio.wasAnyPressed()', 'gpio.wasAnyReleased()', 'gpio.wasTouchActivity()',
              'halTiltSensor.hadActivity()'):
    assert event in normal[input_start:input_end]
assert input_end < normal.index('idleDisplayWhileAwake();')
assert normal.index('idleDisplayWhileAwake();') < normal.index('if (wakePowerReleasePending')
assert loop.count('idleDisplayWhileAwake(') == 2
# Background work and device/frontlight policy must not extend the panel timer.
for forbidden in ('Frontlight', 'enterDeepSleep', 'preventAutoSleep', 'lastActivityTime', 'lastPanelInputTime', 'userInputReceived', 'millis()'):
    assert forbidden not in production
hal = (ROOT / 'lib/hal/HalDisplay.cpp').read_text().split('void HalDisplay::controllerIdle() {')[1].split('\n}')[0]
assert 'HalSpiBus::Lock spiLock;' in hal and 'einkDisplay.controllerIdle();' in hal
harness = r'''
#include <cassert>
#include <climits>
unsigned long now = 0;
unsigned long millis() { return now; }
bool busy = false;
int calls = 0;
struct RenderLock {
 enum class Mode { Try };
 explicit RenderLock(Mode) {}
 bool ownsLock() { return !busy; }
};
struct Display { void controllerIdle() { assert(!busy); ++calls; } } display;
'''
checks = r'''
int main() {
 // Every idle opportunity reaches the driver, even immediately after boot or input.
 now = 0; idleDisplayWhileAwake(); assert(calls == 1);
 now = 100; idleDisplayWhileAwake(); assert(calls == 2);
 now = 499; idleDisplayWhileAwake(); assert(calls == 3);
 busy = true; idleDisplayWhileAwake(); assert(calls == 3);
 busy = false; idleDisplayWhileAwake(); assert(calls == 4);
 // Frequent input frames must not postpone driver-controlled power-off.
 for (now = 500; now < 1000; now += 10) idleDisplayWhileAwake();
 assert(calls == 54);
 now = ULONG_MAX; idleDisplayWhileAwake(); assert(calls == 55);
 now = 0; idleDisplayWhileAwake(); assert(calls == 56);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for a4 in (1, 0):
        test = checks if a4 else '''int main() {
          idleDisplayWhileAwake(); now = 100000;
          idleDisplayWhileAwake(); idleDisplayWhileAwake(); assert(calls == 0);
        }'''
        (tmp / 'test.cpp').write_text(harness + production + test)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        f'-DFREEINK_DEVICE_EEGO_A4={a4}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)
print('EEGO driver-owned idle timing, USB routing, lock exclusion and non-A4 isolation passed')
