#!/usr/bin/env python3
"""Compile the production restart functions against an ordered USB/reset spy."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    source = (ROOT / "src/main.cpp").read_text()
    production = "\n".join(function(source, signature) for signature in (
        "static void restartWithSilentToken(",
        "static void clearSilentRestartReaderPageBuild(",
        "static void silentRestartToHome(",
        "void silentRestart()",
        "void restartToHomeAfterStorageHandoff()",
    ))
    harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>

enum Event { Popup, Delay, Handoff, Restart };
std::vector<Event> events;
uint32_t silentRebootMagic = 0, silentRebootTarget = 0, silentRebootPayload = 0;
uint32_t silentReaderPageBuildMagic = 1, silentReaderPageBuildBookHash = 1;
uint32_t silentReaderPageBuildPackedTarget = 1, silentReaderPageBuildFlags = 1;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_FRONTLIGHT_VALID = 1U << 31;
constexpr uint32_t SILENT_REBOOT_FRONTLIGHT_ON = 1U << 30;
bool deepSleepInProgress = false;
struct Light { bool on = false; bool isOn() const { return on; } } Frontlight;
struct Gui { void drawPopup(int, int) { events.push_back(Popup); } } GUI;
int renderer = 0;
constexpr int STR_LOADING = 0;
int tr(int value) { return value; }
template <typename... Args> void logStub(Args...) {}
#define LOG_DBG(...) logStub(__VA_ARGS__)
void delay(unsigned) { events.push_back(Delay); }
void handoffUsbOtgToSerialJtag() { events.push_back(Handoff); }
struct Esp { void restart() { events.push_back(Restart); } } ESP;
'''
    checks = r'''
int main() {
  for (bool lightOn : {false, true}) {
    Frontlight.on = lightOn;
    silentRestart();
    assert((events == std::vector<Event>{Popup, Delay, Restart}));
    events.clear();
    restartToHomeAfterStorageHandoff();
    assert((events == std::vector<Event>{Popup, Delay, Handoff, Restart}));
    assert(silentRebootMagic == SILENT_REBOOT_MAGIC);
    assert(silentRebootTarget == (SILENT_REBOOT_FRONTLIGHT_VALID |
                                 (lightOn ? SILENT_REBOOT_FRONTLIGHT_ON : 0)));
    assert(silentRebootPayload == 0 && silentReaderPageBuildMagic == 0);
    events.clear();
  }
  deepSleepInProgress = true;
  restartToHomeAfterStorageHandoff();
  assert(events.empty());
}
'''
    with tempfile.TemporaryDirectory(prefix="crossink-usb-restart-") as directory:
        work = Path(directory)
        cpp = work / "restart.cpp"
        cpp.write_text(harness + production + checks)
        program = work / "restart"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(program)], check=True)
        subprocess.run([str(program)], check=True)
    print("USB restart order, ordinary restart, light-state retention and sleep guard passed")


if __name__ == "__main__":
    main()
