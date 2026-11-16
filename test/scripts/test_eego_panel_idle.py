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
assert 'gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity()' in usb
assert loop.index('mappedInputManager.update();') < usb_start
# Normal input includes tilt, sampled before the idle check and shortcut returns.
normal = loop[usb_end:]
assert normal.index('halTiltSensor.update(') < normal.index('const bool userInputReceived')
input_start = normal.index('const bool userInputReceived')
input_end = normal.index(';', input_start)
for event in ('gpio.wasAnyPressed()', 'gpio.wasAnyReleased()', 'gpio.wasTouchActivity()',
              'halTiltSensor.hadActivity()'):
    assert event in normal[input_start:input_end]
assert input_end < normal.index('idleDisplayWhileAwake(userInputReceived);')
assert normal.index('idleDisplayWhileAwake(userInputReceived);') < normal.index('if (wakePowerReleasePending')
assert loop.count('idleDisplayWhileAwake(') == 2
# Background work and device/frontlight policy must not extend the panel timer.
for forbidden in ('Frontlight', 'enterDeepSleep', 'preventAutoSleep', 'lastActivityTime'):
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
int main(int argc, char**) {
 if (argc > 1) {
   // After a slow boot (including USB flashing), do not start another 500-ms wait.
   now = 8000; idleDisplayWhileAwake(false); assert(calls == 1);
   return 0;
 }
 now = 100; idleDisplayWhileAwake(false); assert(calls == 0);
 now = 499; idleDisplayWhileAwake(false); assert(calls == 0);
 now = 500; idleDisplayWhileAwake(false); assert(calls == 1);
 busy = true; now = 700; idleDisplayWhileAwake(false); assert(calls == 1);
 busy = false; idleDisplayWhileAwake(false); assert(calls == 2);
 // Button, touch and tilt events share the same input clock at the call site.
 idleDisplayWhileAwake(true); assert(calls == 2);
 now = 1199; idleDisplayWhileAwake(false); assert(calls == 2);
 now = 1200; idleDisplayWhileAwake(false); assert(calls == 3);
 // Input must reset the timer even when the renderer currently owns the lock.
 busy = true; now = 1600; idleDisplayWhileAwake(true); assert(calls == 3);
 busy = false; now = 2099; idleDisplayWhileAwake(false); assert(calls == 3);
 now = 2100; idleDisplayWhileAwake(false); assert(calls == 4);
 // Repeated static-screen checks do not restart the input timer.
 now = 2150; idleDisplayWhileAwake(false); assert(calls == 5);
 now = ULONG_MAX - 100; idleDisplayWhileAwake(true);
 now = 398; idleDisplayWhileAwake(false); assert(calls == 5);
 now = 399; idleDisplayWhileAwake(false); assert(calls == 6);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for a4 in (1, 0):
        test = checks if a4 else '''int main() {
          idleDisplayWhileAwake(false); now = 100000;
          idleDisplayWhileAwake(false); idleDisplayWhileAwake(true); assert(calls == 0);
        }'''
        (tmp / 'test.cpp').write_text(harness + production + test)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        f'-DFREEINK_DEVICE_EEGO_A4={a4}', str(tmp / 'test.cpp'), '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)
        if a4:
            subprocess.run([str(tmp / 'test'), 'after-usb-boot'], check=True)
print('EEGO upstream idle timing, boot, input routing, lock exclusion, rollover and non-A4 isolation passed')
