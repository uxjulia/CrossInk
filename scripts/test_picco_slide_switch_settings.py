#!/usr/bin/env python3
"""Exercise the Picco touchscreen switch while an EPUB has a font override."""
from pathlib import Path
import subprocess
import tempfile

from test_reader_settings_transitions import ROOT, method

FIXTURE = r'''
#include <cassert>
#include "util/SlideSwitchToggle.h"

struct ReaderSettingsSnapshot { int fontSize; };
struct CrossPointSettings {
  enum class SlideSwitchAction { Light, NightMode, TouchControls, TiltPageTurn };
  SlideSwitchAction slideSwitchAction = SlideSwitchAction::TouchControls;
  enum { TILT_OFF, TILT_ON };
  int tiltPageTurn = TILT_OFF;
  bool frontlightOn = false, screenInverted = false, disableReaderTouchscreen = false;
  ReaderSettingsSnapshot active{16}, saved{14};
  bool savedTouchDisabled = false;
  bool saveToFile() {
    saved = active;
    savedTouchDisabled = disableReaderTouchscreen;
    return true;
  }
} SETTINGS;
void captureReaderSettings(ReaderSettingsSnapshot& out) { out = SETTINGS.active; }
void applyReaderSettings(const ReaderSettingsSnapshot& in) { SETTINGS.active = in; }

struct EpubReaderActivity {
  ReaderSettingsSnapshot globalReaderSettingsBeforeBook{14};
  bool restoreGlobalReaderSettingsOnExit = true, bookReaderSettingsSuspendedForGlobalEdit = false;
  bool saveGlobalSettingsPreservingBookOverrides();
  void persistGlobalSettings() { saveGlobalSettingsPreservingBookOverrides(); }
} reader;

struct ActivityManager {
  EpubReaderActivity* currentActivity = &reader;
  EpubReaderActivity* findEpubReader() { return currentActivity; }
  void persistGlobalSettings();
  void requestUpdate() {}
} activityManager;

SlideSwitchToggle piccoSlideSwitch;
struct { bool on = false; bool hasToggleSwitch(){return true;} bool isToggleSwitchOn(){return on;} } gpio;
struct { bool present(){return false;} void setOn(bool){} bool isOn(){return false;} } Frontlight;
struct { bool isAvailable(){return false;} void clearPendingEvents(){} } halTiltSensor;
#define LOG_ERR(...) ((void)0)
'''

CASES = r'''
int main() {
  assert(!handleSlideSwitch());
  for (int movement = 0; movement < 2; ++movement) {
    gpio.on = !gpio.on;
    assert(handleSlideSwitch());
    assert(SETTINGS.savedTouchDisabled == gpio.on);
    assert(SETTINGS.saved.fontSize == 14);
    assert(SETTINGS.active.fontSize == 16);
  }
}
'''


def main():
    production = method("src/activities/reader/EpubReaderActivity.cpp",
                        "bool EpubReaderActivity::saveGlobalSettingsPreservingBookOverrides()")
    production += method("src/activities/ActivityManager.cpp", "void ActivityManager::persistGlobalSettings()")
    production += method("src/main.cpp", "static bool handleSlideSwitch()")
    with tempfile.TemporaryDirectory(prefix="picco-switch-settings-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(FIXTURE + production + CASES)
        subprocess.run(["c++", "-std=c++17", "-I", str(ROOT / "src"), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: Picco saves both touchscreen toggles and preserves the global and active EPUB font sizes")


if __name__ == "__main__":
    main()
