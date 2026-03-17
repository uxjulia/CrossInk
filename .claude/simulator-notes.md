# Simulator Development Context

Branch: `feature/simulator`

## What We're Building

A macOS desktop simulator for CrossPoint firmware. Instead of flashing to the ESP32-C3 device, the simulator compiles the firmware as a native macOS binary using PlatformIO's `env:simulator` environment. It renders the e-ink display via SDL2 in a window.

## What Was Done This Session

### Build Errors Fixed (in order)

All new/modified mock files live in `lib/simulator_mock/src/` unless noted otherwise.

1. **`HalSystem.h`** — Added missing `void restart()` to the `HalSystem` namespace.
2. **`HalGPIO.cpp`** — Removed out-of-line `HalGPIO::HalGPIO() {}` (header declares `= default`).
3. **`esp_mac.h`** _(new)_ — Stub for `esp_efuse_mac_get_default()` returning a fixed fake MAC.
4. **`Arduino.h`** — Added `#include <cassert>` and `micros()` (same pattern as `millis()`).
5. **`base64.h`** — Replaced dummy stub with a real encoder accepting `(const uint8_t*, size_t)`.
6. **`mbedtls/base64.h`** _(new)_ — Pure C++ `mbedtls_base64_decode` (no Homebrew dependency).
7. **`HardwareSerial.h`** — Added `#include "Arduino.h"` so Arduino globals propagate everywhere via `Logging.h`.
8. **`JPEGDEC.h`** _(new)_ — No-op stub; `open()` returns 0 so callers bail out gracefully.
9. **`PNGdec.h`** _(new)_ — No-op stub; `open()` returns `PNG_INVALID_FILE`.
10. **`platformio.ini`** — Added `PNGdec, JPEGDEC` to `lib_ignore` for simulator; removed from `lib_deps`; removed `patch_jpegdec.py`.
11. **`WiFiClientSecure.h`** _(new)_ — `using WiFiClientSecure = NetworkClientSecure`.
12. **`MD5Builder.h`** _(new)_ — Real MD5 via macOS `CommonCrypto/CommonDigest.h`.
13. **`HTTPClient.h`** — Added `setAuthorization()`, `PUT(const char*)`, `PUT(const String&)`.
14. **`WiFi.h`** — Added `using WiFiClient = NetworkClient`.
15. **`qrcode.h`** — Added `#include <cstdint>`.
16. **`qrcode.cpp`** _(new)_ — Stub implementations of `qrcode_getBufferSize/initText/getModule`.
17. **`ESP.cpp`** — Added `WiFiClass WiFi;` and `MDNSClass MDNS;` global definitions.
18. **`lib/Logging/Logging.cpp`** — Added `MySerialImpl::instance` definition + `write()`/`flush()` bodies under `#ifdef SIMULATOR`.
19. **`src/network/OtaUpdater.cpp`** — Added `#ifdef SIMULATOR` stub branch returning `NO_UPDATE` for all methods.
20. **`lib/uzlib/src/uzlib_checksums.c`** _(new)_ — Implements `uzlib_adler32` and `uzlib_crc32`.

### Runtime Fix: Black Screen

21. **`HalDisplay.cpp`** — `clearScreen` was writing to the SDL pixel array instead of the framebuffer. Fixed to `memset(getFrameBuffer(), color, BUFFER_SIZE)`.

### Runtime Fix: Sideways Rendering

The renderer defaults to `Portrait`, applying a 90° CW coordinate rotation to the physical 800×480 framebuffer. Showing the raw buffer in SDL made everything appear sideways.

- **`HalDisplay.cpp`** — `refreshDisplay` now converts the framebuffer to pixels and sets an atomic `pendingPresent` flag but does NOT call SDL (SDL must run on the main thread on macOS).
- **`HalDisplay.cpp`** — New `presentIfNeeded()`: called from the main thread; uploads pixels, applies `SDL_RenderCopyEx` rotation (−90° for Portrait, +90° for PortraitInverted, none for Landscape), presents, drains SDL events.
- **`HalDisplay.h`** — Added `setSimulatorOrientation(int)` and `presentIfNeeded()`.
- **`GfxRenderer.h`** — `setOrientation()` calls `display.setSimulatorOrientation()` under `#ifdef SIMULATOR`.
- **`simulator_main.cpp`** — Calls `display.presentIfNeeded()` after each `loop()`.

### Runtime Fix: Portrait Upside-Down and Stretched

Portrait content appeared upside-down and distorted. Two bugs in `presentIfNeeded()`:

1. **Wrong rotation direction** — `rotateCoordinates` for `Portrait` stores content rotated 90° CCW in the physical buffer (`phyX = y`, `phyY = 479 - x`). To undo this in SDL, `+90°` (CW) is needed, not `−90°`. `Portrait` and `PortraitInverted` had their angles swapped.
2. **Wrong dst rect** — `SDL_RenderCopyEx` rotates around the _centre_ of the dst rect. The rect was portrait-shaped `{0, 0, 240, 400}` but must be landscape-shaped so that after rotation it fills the portrait window. Correct rect: `{(H−W)/4, (W−H)/4, W/2, H/2}` = `{−80, 80, 400, 240}` (where W=800, H=480).

Final rotation table:
| Orientation | `rotateCoordinates` transform | SDL angle |
|---|---|---|
| `Portrait` | 90° CCW into physical buffer | `+90.0` |
| `PortraitInverted` | 90° CW into physical buffer | `−90.0` |
| `Landscape*` | direct / 180° | `0` (no rotation) |

### Rendering Quality: Dithering and HiDPI

Bayer-dithered UI elements (e.g. dark nav bars) rendered as harsh black/white stripes instead of gray at 0.5× scale with nearest-neighbour sampling. SDL window was also blurry on Retina because HiDPI was not requested.

- **`SDL_WINDOW_ALLOW_HIGHDPI`** — added to `SDL_CreateWindow` flags so macOS allocates a full Retina drawable (480×800 physical pixels for a 240×400 logical portrait window).
- **`SDL_RenderSetLogicalSize(sdl_renderer, winW, winH)`** — pins rendering coordinate space to logical window size; SDL handles the HiDPI upscale transparently.
- **`SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1")`** — enables bilinear filtering on the texture (must be set _before_ `SDL_CreateTexture`). Dithered patterns now average to correct gray.

### Graceful Exit (no crash dialog on window close)

Calling `exit(0)` from the SDL event handler while the background render thread was blocked in `ulTaskNotifyTake` caused macOS to show "Program quit unexpectedly" (likely from the C++ runtime calling `std::terminate` or an SDL atexit race).

- **`HalDisplay.cpp`** — Replaced `exit(0)` with `quitRequested.store(true)` (static `std::atomic<bool>`).
- **`HalDisplay.h`** — Added `bool shouldQuit() const`.
- **`simulator_main.cpp`** — Changed `while (true)` to `while (!display.shouldQuit())`; calls `SDL_Quit()` then `return 0` for clean teardown.

### Runtime Fix: File Browser Shows No Books

`FileBrowserActivity` scans for books using SdFat-style directory iteration (`open` → `isDirectory` → `rewindDirectory` → `openNextFile` → `getName`). All four were no-op stubs in the mock.

- **`HalStorage.cpp`** — `HalFile::Impl` extended with a `DIR*` and `openAsDir()` method.
- **`HalStorage::open()`** — `stat()`s the path; calls `openAsDir` for directories, existing file-open for files.
- **`HalFile::isDirectory()`** — returns `true` when `DIR*` is set.
- **`HalFile::rewindDirectory()`** — calls `rewinddir()`.
- **`HalFile::openNextFile()`** — calls `readdir()`, skips `.`/`..`, `stat()`s each entry to determine dir vs file, returns a child `HalFile` opened appropriately.
- **`HalFile::getName()`** — extracts the final path component (filename only) from `impl->path`.
- **`HalFile::close()`** — now closes both `DIR*` and `fstream`.

Books must be placed at `./fs_/books/` relative to the simulator binary's working directory (maps to SD card path `/books/`).

### Runtime Fix: Stuck on Boot Screen

`ActivityManager::begin()` creates a FreeRTOS render task. Old mocks made `xTaskCreate` a no-op, so `renderTaskLoop` never ran and `HomeActivity::render()` was never called.

- **`freertos/FreeRTOS.h`** — `TaskHandle_t` is now `SimTaskHandle*`: a struct with a `std::thread`, `std::mutex`, `std::condition_variable`, and notify counter.
- **`freertos/task.h`** — `xTaskCreate` launches a real `std::thread`; `ulTaskNotifyTake` blocks on the condvar; `xTaskNotify` signals it. `thread_local SimTaskHandle* tl_currentTaskHandle` lets each task thread find its own handle.
- **`freertos/semphr.h`** — `SemaphoreHandle_t` is now `SimMutex*` wrapping a `std::recursive_mutex`.

### Runtime Fix: Ebook Reader Shows Nothing (Three Bugs)

Opening an ebook did nothing visible. `EpubReaderActivity::render()` called `createSectionFile()` which silently failed. Three root causes in `HalStorage.cpp`:

**Bug 1 — `std::fstream` EOF/seek (`stream.clear()` missing)**
`ZipFile::loadZipDetails()` reads the last 1KB of the epub to find the EOCD record, setting `eofbit`. All subsequent `seekg()` calls silently did nothing (standard behaviour when `eofbit` is set). Fixed: added `impl->stream.clear()` before every `seekg`, `tellg`, `read`, and `write` call.

**Bug 2 — Write-only files use `tellg()` which is invalid**
`std::fstream` opened with `out`-only mode has no valid get-pointer. `HalFile::position()` called `tellg()`, returning -1 (cast to `uint32_t` → `0xFFFFFFFF`). `Section::onPageComplete()` uses `file.position()` to record each page's offset in the LUT. With garbage offsets, `loadPageFromSectionFile()` sought to offset 4GB and returned `nullptr`. Fixed: `position()` now tries `tellp()` first.

**Bug 3 — Write files opened without `in` mode**
Opening with `out`-only prevents `seekg/tellg` from working at all. Fixed: `O_WRONLY` now opens with `in | out` so both get/put pointers track the same underlying OS file offset. This also makes `size()` correct for write-mode files.

**Symptom of all three bugs**: "double press required" — first press opened the reader but rendered nothing (screen unchanged), user pressed again thinking nothing happened, which then opened the reader menu on top of the invisible reader content.

**Resolution**: All three fstream bugs were ultimately eliminated by rewriting `HalFile::Impl` to use POSIX file descriptors (`::open`, `::read`, `::write`, `lseek`, `fsync`, `::close`) instead of `std::fstream`. POSIX fds have no EOF state, no separate get/put pointers, and no mode restrictions on seek — eliminating all three classes of bug at once.

### Runtime Fix: HalStorage POSIX Flag Translation (Fourth Bug)

After the POSIX fd rewrite, section cache files (`.crosspoint/epub_.../spine.bin.tmp`) still failed to open for writing. Root cause: the flag translation in `HalFile::Impl::open()` was translating SdFat flag values (CREAT=0x10, TRUNC=0x20) to POSIX, but the simulator's `FsApiConstants.h` just `#include <fcntl.h>` with `typedef int oflag_t` — so callers already pass native POSIX values (CREAT=0x200, TRUNC=0x400 on macOS). The translation stripped the CREAT/TRUNC bits, causing `::open()` to receive only `O_WRONLY` with no create/truncate flags.

Fixed: removed the flag translation entirely. `HalFile::Impl::open()` now passes flags straight through to `::open()`.

### Diagnostic: LOG Output Redirected to stderr

All `LOG_DBG`/`LOG_ERR` output was going to `stdout` via `HWCDC::write → std::cout`, while `[SIM] open failed` messages go to `stderr`. This made LOG messages invisible when only viewing stderr. Fixed `HardwareSerial.h`: `HWCDC::write()` now uses `std::cerr`, and `HWCDC::printf()` now actually formats and prints the string (previously was a no-op stub printing "HWCDC printf called").

### Runtime Fix: BookMetadataCache `lutOffset` size_t vs uint32_t (Fifth Bug)

After all storage fixes, ebook metadata loaded but spine entries returned empty hrefs. Root cause: `BookMetadataCache::lutOffset` was declared as `size_t` (8 bytes on macOS 64-bit), but `headerASize` in `buildBookBin()` computed it as `sizeof(uint32_t)` (4 bytes). This 4-byte mismatch caused `lutOffset` to be stored at the wrong value in `book.bin`, making all spine entry seeks land 4 bytes before the actual data.

Fixed: changed `size_t lutOffset` → `uint32_t lutOffset` in `BookMetadataCache.h`. On ESP32 (32-bit) `size_t == uint32_t` so no device-side impact; only the macOS simulator was affected.

**After all fixes**: delete `./fs_/.crosspoint/` (stale caches built with old code), rebuild, open book → "Indexing..." popup appears → page renders. Book cover, title, and author display correctly on the home screen.

## Current State

The simulator builds and runs. Portrait orientation is correct. The home screen renders with readable text and proper gray shading. File browsing works — books placed in `./fs_/books/` are listed. Window close exits cleanly without a crash dialog. Ebook reading works — first open shows an "Indexing..." popup while the section cache is built, then the first page renders. Book cover images, titles, and authors display correctly.

## Key File Locations

| Purpose                     | Path                                        |
| --------------------------- | ------------------------------------------- |
| Simulator mock sources      | `lib/simulator_mock/src/`                   |
| FreeRTOS mocks              | `lib/simulator_mock/src/freertos/`          |
| PlatformIO simulator env    | `platformio.ini` `[env:simulator]`          |
| SDL display impl            | `lib/simulator_mock/src/HalDisplay.cpp`     |
| SDL input impl              | `lib/simulator_mock/src/HalGPIO.cpp`        |
| Simulator entry point       | `lib/simulator_mock/src/simulator_main.cpp` |
| Filesystem root (simulator) | `./fs_/` (relative to binary working dir)   |

## Known Remaining Work

- SDL window size is fixed at half-scale; no runtime resize when orientation changes.
- Thread safety is minimal — relies on `std::recursive_mutex` in `RenderLock`.
- Verify `HalPowerManager::startDeepSleep` doesn't trigger on `WakeupReason::Other` (it shouldn't).
- Exiting back to the book after going into Reader Options from the in-book menu causes the reader's position to be inaccurate.

# Button Mapping

Key Button Logical action (default mapping)
↑ Up arrow BTN_UP Page back (side button)
↓ Down arrow BTN_DOWN Page forward (side button)
← Left arrow BTN_LEFT Left front button
→ Right arrow BTN_RIGHT Right front button
Return BTN_CONFIRM Confirm / Select
Escape BTN_BACK Back
P BTN_POWER Power
