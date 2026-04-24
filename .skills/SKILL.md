# CrossPoint Reader — Agent Guide

Project: Open-source e-reader firmware for Xteink X4 (ESP32-C3).

## Agent Rules

- Role: Senior Embedded Systems Engineer (ESP-IDF/Arduino-ESP32 specialized).
- **380KB RAM is the hard ceiling.** Stability is non-negotiable.
- Cite file path + line numbers before proposing any change.
- Do not assume ESP-IDF API availability — verify in `open-x4-sdk/` or docs.
- Justify every heap allocation or explain why stack/static was rejected.
- After suggesting a fix, tell the user how to verify it on hardware.

## Session Setup

- Read `.claude/CONTEXT-*.md` files at session start for prior context.
- When any important updates have been made during a session, make sure to update this file.
- When I say "Summarize session" → create `.claude/CONTEXT-YYYY-MM-DD.md` with relevant findings, code refs, and file:line pointers.
- Detect platform: `uname -s` → `MINGW64_NT-*` (Windows Git Bash), `Darwin` (macOS), `Linux`.

---

## Hardware

- MCU: ESP32-C3 (single-core RISC-V @ 160MHz) — **NO PSRAM**
- RAM: ~380KB usable. Single framebuffer only (48,000 bytes, 800×480÷8).
- Flash: 16MB. Display: 800×480 E-Ink, 1–2s full refresh.
- Storage: SD card via SdFat. **One reader per file at a time on real hardware.**

---

## Resource Protocol

1. **Stack**: Local variables < 256 bytes. Use `std::unique_ptr` or static pools for larger.
2. **Heap fragmentation**: Allocate buffers once in `onEnter()`, reuse. No `new`/`delete` in loops.
3. **Flash**: Large constant data must be `static const` (stays in Flash, frees DRAM).
4. **Strings**: No `std::string` / Arduino `String` in hot paths. Use `string_view` (read-only) or `snprintf` + `char[]`.
5. **UI strings**: Always `tr(STR_*)` macro. Never hardcode user-facing text. Log messages may be hardcoded.
6. **`constexpr` first**: All compile-time constants must be `constexpr` (not just `static const`). Use `static constexpr` for class members.
7. **`std::vector`**: Always `.reserve(N)` before any `push_back()` loop.
8. **SPIFFS writes**: Guard with value-change check (`if (newVal == _current) return;`). Debounce progress saves — write on activity exit or every N pages, not every turn.

---

## Build System

PlatformIO (`platformio.ini` committed; `platformio.local.ini` gitignored for personal overrides).

- Standard: C++20 (`-std=c++2a`). No exceptions, no RTTI.
- Logging: `LOG_INF` / `LOG_DBG` / `LOG_ERR` from `Logging.h`. Raw Serial deprecated.
- Environments: `default` (LOG_LEVEL=2), `gh_release` (LOG_LEVEL=0), `gh_release_rc` (LOG_LEVEL=1), `slim` (no serial).
- Monitor: `python3 scripts/debugging_monitor.py` (color-coded) or `pio device monitor`.

Key build flags (in `platformio.ini`):

```
-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1   // Single framebuffer — grayscale needs storeBwBuffer()/restoreBwBuffer()
-DXML_CONTEXT_BYTES=1024              // XML parser memory limit
-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1   // Avoid zlib conflicts
-DXML_GE=0                           // Disable XML general entities
```

---

## Directory Structure

```
lib/                   Internal libraries (Epub engine, GfxRenderer, UITheme, I18n)
lib/hal/               HAL layer — ALWAYS use HAL, never SDK directly
lib/I18n/translations/ Source YAML files for i18n (english.yaml is reference)
src/activities/        UI logic (Activity Lifecycle: onEnter, loop, onExit)
src/fontIds.h          Font ID constants
open-x4-sdk/           Low-level SDK (EInkDisplay, InputManager, etc.)
data/html/             Source HTML templates (NOT the generated headers)
docs/file-formats.md   Cache file format versions
.crosspoint/           SD card binary cache (EPUB metadata + layout sections)
```

---

## HAL Layer

**Always use HAL classes — never SDK classes directly.**

| HAL Class    | Wraps           | Singleton |
| ------------ | --------------- | --------- |
| `HalDisplay` | `EInkDisplay`   | —         |
| `HalGPIO`    | `InputManager`  | —         |
| `HalStorage` | `SDCardManager` | `Storage` |

File I/O uses `FsFile` (SdFat), not Arduino `File`. Always call `file.close()` explicitly.

---

## Singletons

```cpp
SETTINGS   // CrossPointSettings::getInstance()
APP_STATE  // CrossPointState::getInstance()
GUI        // UITheme::getInstance()
Storage    // HalStorage::getInstance()
I18N       // I18n::getInstance()
```

---

## Coding Standards

- Naming: `PascalCase` classes, `camelCase` methods/vars, `UPPER_SNAKE_CASE` constants, no prefix for private members.
- `#pragma once` for all headers.
- Prefer `std::unique_ptr`. Avoid `std::shared_ptr` (atomic overhead, single-core).
- Avoid `std::function<>` and excessive templates in library/render-loop code — use raw function pointers instead.

### ESP32-C3 Pitfalls

**`string_view` null-termination**: `string_view` is not null-terminated. Never pass `.data()` to C APIs. Use `std::string(view).c_str()` or `snprintf(buf, sizeof(buf), "%.*s", ...)`.

**`IRAM_ATTR`**: ISR handlers must be `IRAM_ATTR`. Data they read must be `DRAM_ATTR` (a `static const` in flash will fault during cache suspension).

**ISR/task sync**: `xSemaphoreTake()` cannot be called from ISR. Use `xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()` for ISR→task signaling.

**RISC-V alignment**: Never cast `uint8_t*` to a wider pointer type. Use `memcpy` for unaligned reads. Applies to cache deserialization and packed structs.

### Error Handling

No exceptions. No `abort()`. Always log before error return.

1. `LOG_ERR("MOD", "reason"); return false;` — 90% of cases
2. `LOG_ERR` + fallback to default
3. `assert(false)` — fatal impossible states only
4. `ESP.restart()` — OTA recovery only

### malloc

Acceptable for: large temp buffers (>256 bytes), one-time activity init, variable-size bitmap buffers.
Always: check for `nullptr`, free immediately after use, set to `nullptr` after free.

---

## Activity Lifecycle

Activities are heap-allocated and deleted on exit. Resources allocated in `onEnter()` **must** be freed in `onExit()`.

```cpp
void onEnter()  { Activity::onEnter(); /* alloc buffers, spawn tasks */ }
void loop()     { /* handle input */ }
void onExit()   { /* vTaskDelete, free buffers, close files */ Activity::onExit(); }
```

Free in reverse order. `vTaskDelete()` **before** activity destruction. FreeRTOS task stacks: 2048 bytes (simple), 4096 (network/EPUB parsing).

**Memory Implications**:

- Activity navigation = `delete` old activity + `new` create next activity
- Any memory allocated in `onEnter()` MUST be freed in `onExit()`
- FreeRTOS tasks MUST be deleted in `onExit()` before activity destruction
- File handles MUST be closed in `onExit()`

**Activity Pattern**:

```cpp
void onEnter()  { Activity::onEnter(); /* alloc: buffer, tasks */ render(); }
void loop()     { mappedInput.update(); /* handle input */ }
void onExit()   { /* free: vTaskDelete, free buffer, close files */ Activity::onExit(); }
```

**Critical**: Free resources in reverse order. Delete tasks BEFORE activity destruction.

### FreeRTOS Task Guidelines

**Source**: [src/activities/util/KeyboardEntryActivity.cpp:45-50](../src/activities/util/KeyboardEntryActivity.cpp)

**Pattern**: See Activity Lifecycle above. `xTaskCreate(&taskTrampoline, "Name", stackSize, this, 1, &handle)`

**Stack Sizing** (in BYTES, not words):

- **2048**: Simple rendering (most activities)
- **4096**: Network, EPUB parsing
- Monitor: `uxTaskGetStackHighWaterMark()` if crashes

**Rules**: Always `vTaskDelete()` in `onExit()` before destruction. Use mutex if shared state.

### Global Font Loading

**Source**: [src/main.cpp:40-115](../src/main.cpp)

**All fonts are loaded as global static objects** at firmware startup:

- Noto Serif: 12, 14, 16, 18pt (4 styles each: regular, bold, italic, bold-italic)
- Noto Sans: 12, 14, 16, 18pt (4 styles each)
- OpenDyslexic: 8, 10, 12, 14pt (4 styles each)
- Ubuntu UI fonts: 10, 12pt (2 styles)

**Total**: ~80+ global `EpdFont` and `EpdFontFamily` objects

**Compilation Flag**:

```cpp
#ifndef OMIT_FONTS
  // Most fonts loaded here
#endif
```

**Implications**:

- Fonts stored in **Flash** (marked as `static const` in `lib/EpdFont/builtinFonts/`)
- Font rendering data cached in **DRAM** when first used
- `OMIT_FONTS` can reduce binary size for minimal builds
- Font IDs defined in [src/fontIds.h](../src/fontIds.h)

**Usage**:

```cpp
#include "fontIds.h"

renderer.insertFont(FONT_UI_MEDIUM, ui12FontFamily);
renderer.drawText(FONT_UI_MEDIUM, x, y, "Hello", true);
```

---

## UI

- Never hardcode 800 or 480 — use `renderer.getScreenWidth()` / `getScreenHeight()`.
- Viewable area: `renderer.getOrientedViewableTRBL()`.
- All rendering via `GUI` macro (UITheme). No hardcoded fonts, colors, or positions.
- Button input: use `MappedInputManager::Button::*` enums. Never raw `HalGPIO::BTN_*` (except ButtonRemapActivity).

---

## Generated Files — Never Edit Directly

| File(s)                                                   | Generator                                                    | Source to edit                 |
| --------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------ |
| `src/network/html/*.generated.h`                          | `scripts/build_html.py` (pre-build)                          | `data/html/*.html`             |
| `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp` | `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` | `lib/I18n/translations/*.yaml` |

**Commit source files only.** Generated files are in `.gitignore`.

To add/modify translations: edit YAML → run gen script → build. English (`english.yaml`) is the reference; missing keys fall back to English.

---

## Cache Management

Location on SD: `.crosspoint/epub_<hash>/` (hash = `std::hash<std::string>{}(filepath)`).

**Auto-invalidated when**: file format version changes, render settings change (font, spacing, margins), orientation/resolution changes, book file moved/renamed.

**Manual clear**: delete `.crosspoint/epub_<hash>/` or all of `.crosspoint/`.

**Current file format versions** (increment BEFORE changing binary structure):

- `book.bin`: Version 5
- `section.bin`: Version 21

After incrementing a version, document the change in `docs/file-formats.md`.

---

## CI/CD

| Workflow      | File                                        |
| ------------- | ------------------------------------------- |
| Build check   | `.github/workflows/ci.yml`                  |
| Format check  | `.github/workflows/pr-formatting-check.yml` |
| Release build | `.github/workflows/release.yml`             |
| RC build      | `.github/workflows/release_candidate.yml`   |

Fix CI failures before requesting review. Format failures → run clang-format locally.

---

Philosophy: Dedicated e-reader, not a Swiss Army knife. If a feature adds RAM pressure without meaningfully improving the reading experience, it is out of scope.
