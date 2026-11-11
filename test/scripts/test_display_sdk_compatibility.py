"""Compile the production HAL adapters against upstream and extended SDK APIs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "lib/hal/HalDisplay.cpp").read_text()
helpers = source.split("namespace {", 1)[1].split("}  // namespace", 1)[0]
harness = r'''
#include <cassert>
#include <cstdint>
struct UpstreamDisplay {};
struct ExtendedDisplay {
  bool reading = false, painted = false, restored = false;
  unsigned calls = 0;
  bool restoreVisibleFrame() { return true; }
  void setReaderRefresh(bool value) { reading = value; }
  bool displayRetainedGrayscaleOverlay(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool restore) {
    assert(x == 2 && y == 3 && w == 4 && h == 5);
    restored = restore;
    ++calls;
    return painted;
  }
};
'''
checks = r'''
int main() {
  UpstreamDisplay upstream;
  setReaderRefreshIfSupported(upstream, true);
  assert(!restoreVisibleFrameIfSupported(upstream));
  assert(!retainedGrayscaleOverlayIfSupported(upstream, 2, 3, 4, 5, true));
  ExtendedDisplay extended;
  assert(restoreVisibleFrameIfSupported(extended));
  setReaderRefreshIfSupported(extended, true);
  assert(extended.reading);
  setReaderRefreshIfSupported(extended, false);
  assert(!extended.reading);
  assert(!retainedGrayscaleOverlayIfSupported(extended, 2, 3, 4, 5, false));
  extended.painted = true;
  assert(retainedGrayscaleOverlayIfSupported(extended, 2, 3, 4, 5, true));
  assert(extended.restored && extended.calls == 2);
}
'''
with tempfile.TemporaryDirectory(prefix="display-sdk-compatibility-") as directory:
    root = Path(directory)
    cpp = root / "test.cpp"
    cpp.write_text(harness + helpers + checks)
    exe = root / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Display SDK compatibility: upstream fallbacks and extension forwarding passed")
