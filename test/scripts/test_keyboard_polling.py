#!/usr/bin/env python3
"""Compile the production background-poll block with a busy-renderer model.

Checks scheduling and preserved lock ownership, not physical display timing.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/main.cpp").read_text()
end = source.index("  const unsigned long activityStartTime = millis();")
start = source.rindex("  if (!activityManager.isReaderActivity()", 0, end)
block = source[start:end]
harness = r'''
#include <cassert>
#include <cstring>
struct {
  bool reader = false, keyboard = false;
  bool isReaderActivity() const { return reader; }
  bool hasActivityNamed(const char* name) const {
    assert(std::strcmp(name, "KeyboardEntry") == 0);
    return keyboard;
  }
} activityManager;
bool busy = false, locked = false;
int waits = 0, polls = 0;
struct RenderLock {
  enum class Mode { Blocking, Try };
  bool owned;
  explicit RenderLock(Mode mode = Mode::Blocking) : owned(!busy) {
    if (busy && mode == Mode::Blocking) { ++waits; owned = true; }
    if (owned) locked = true;
  }
  bool ownsLock() const { return owned; }
  ~RenderLock() { if (owned) locked = false; }
};
namespace pluginevents {
void pollOnline() { assert(locked); ++polls; }
}
void tick() {
''' + block + r'''
}
int main() {
  // Busy render: no wait and no work. Delivery resumes on the next free loop.
  busy = true; tick(); assert(waits == 0 && polls == 0);
  busy = false; tick(); assert(polls == 1 && !locked);
  // Open keyboard: defer synchronous network delivery even between refreshes.
  activityManager.keyboard = true;
  tick(); assert(polls == 1);
  busy = true; tick(); assert(waits == 0 && polls == 1);
  // Closing keyboard allows pending work to run again under the lock.
  activityManager.keyboard = false; busy = false;
  tick(); assert(polls == 2 && !locked);
  activityManager.reader = true; tick(); assert(polls == 2);
}
'''
with tempfile.TemporaryDirectory(prefix="keyboard-polling-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    cpp.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Keyboard polling regression checks passed")
