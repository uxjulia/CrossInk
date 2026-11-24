#!/usr/bin/env python3
"""Run USB Drive's production loop/exit code against storage and reset spies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (ROOT / 'src/activities/network/UsbDriveActivity.cpp').read_text()
production = '\n'.join(function(source, signature) for signature in (
    'void UsbDriveActivity::loop()', 'void UsbDriveActivity::restartToHome()'))
harness = r'''
#include <cassert>
#include <cstdint>
using Time = unsigned long;
Time now = 0;
Time millis() { return now; }
void delay(unsigned) {}
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
struct MappedInputManager { enum class Button { Back, Power }; };
struct Input {
  bool wasPressed(MappedInputManager::Button) { return false; }
  bool wasHomeGesture() { return false; }
};
struct TouchHeaderBackButton { static bool wasTapped(Input&, int) { return false; } };
enum class State { Unsupported, WaitingForHost, Connected, Accessed, Ejected, Disconnected, IoError };
struct StorageSpy {
  State state = State::Connected;
  bool finishOk = true, mounted = false, suspended = true;
  unsigned finishes = 0, disconnects = 0;
  State usbDriveState() { return state; }
  bool ready() { return mounted; }
  bool endUsbDrive() { ++finishes; return finishOk; }
  bool disconnectUsbDriveHost() { ++disconnects; return true; }
  bool usbDriveHostSuspended() { return suspended; }
} Storage;
struct Manager { unsigned homes = 0; void goHome() { ++homes; } } activityManager;
unsigned restarts = 0;
void restartToHomeAfterStorageHandoff() { ++restarts; }
struct UsbDriveActivity {
  using State = ::State;
  State state = State::Connected;
  bool startFailed = false, restartRequested = false, forcedDisconnectRequested = false;
  Time hostWaitStartedAt = 0, startFailureStartedAt = 0, forcedDisconnectRequestedAt = 0;
  static constexpr Time HOST_WAIT_TIMEOUT_MS = 300000, START_FAILURE_TIMEOUT_MS = 30000;
  static constexpr Time FORCED_DISCONNECT_TIMEOUT_MS = 1000;
  Input mappedInput;
  int renderer = 0;
  void requestUpdate() {}
  void loop();
  void restartToHome();
};
'''
checks = r'''
int main() {
  UsbDriveActivity activity;
  // A mounted sleeping host retains ownership even after a full day.
  for (Time time : {1000UL, 3000UL, 3600000UL, 86400000UL}) {
    now = time;
    activity.loop();
    assert(restarts == 0 && Storage.finishes == 0 && Storage.disconnects == 0);
  }
  Storage.finishOk = false;
  Storage.state = State::Ejected;
  activity.loop();
  assert(restarts == 0 && Storage.finishes == 1 && !activity.restartRequested);
  Storage.state = State::IoError;
  activity.loop();
  assert(Storage.disconnects == 1 && activity.forcedDisconnectRequested);
  now += 1000;
  activity.loop();
  assert(restarts == 0 && Storage.finishes == 2);
  activity.loop();
  assert(Storage.finishes == 2);  // failed flush is retried at a bounded interval
  Storage.finishOk = true;
  now += 1000;
  activity.loop();
  assert(restarts == 1 && Storage.finishes == 3);
  activity.loop();
  assert(restarts == 1);
  UsbDriveActivity failedStart;
  failedStart.startFailed = true;
  Storage.mounted = true;
  failedStart.restartToHome();
  assert(activityManager.homes == 1 && restarts == 1 && Storage.finishes == 3);
}
'''
with tempfile.TemporaryDirectory(prefix='crossink-usb-storage-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text('#include <initializer_list>\n' + harness + production + checks)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: sleeping host retains ownership, failed card flush prevents reset, retry and startup failure routing')
