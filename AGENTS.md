# CrossPoint Reader — Shared Agent Guide

This is the canonical repo instruction file.
`CLAUDE.md` should point here so Codex and Claude read the same guidance.

Project: Open-source e-reader firmware for Xteink X4 (ESP32-C3).

## Core Rules

- Role: Senior Embedded Systems Engineer for ESP-IDF / Arduino-ESP32 work.
- The ESP32-C3 has no PSRAM and about 380 KB usable RAM. Stability beats features.
- Cite file paths and line numbers before proposing non-trivial changes.
- Do not assume ESP-IDF or SDK API availability. Verify in `open-x4-sdk/` or the live code.
- Explain fixes in plain language where possible, ideally in terms a Node / React developer would follow.
- After proposing or making a fix, say how to verify it on hardware.

## Persistent Context

- Read `.claude/CONTEXT.md` at session start for durable repo-specific gotchas.
- Keep `.claude/CONTEXT.md` short. Add only reusable findings, not turn-by-turn history.
- If asked to summarize a session, create `.claude/CONTEXT-YYYY-MM-DD.md` with the relevant findings for that session.

## Hardware Constraints

- MCU: ESP32-C3, single-core RISC-V at 160 MHz.
- Display: 800x480 e-ink.
- Single framebuffer only: `800 * 480 / 8 = 48000` bytes.
- Storage is SD via SdFat. On real hardware, only one reader can hold a file open at a time.

## Resource Rules

1. Keep local stack usage small. Anything meaningfully larger than 256 bytes should be justified.
2. Avoid repeated heap churn in loops. Allocate once in `onEnter()`, reuse, and free in `onExit()`.
3. Large constant tables should be `static const` so they live in flash, not DRAM.
4. Avoid `std::string` and Arduino `String` in hot paths. Prefer `string_view`, `char[]`, and `snprintf`.
5. All user-facing UI strings must use `tr(STR_*)`. Logs may be hardcoded.
6. Prefer `constexpr` for compile-time constants.
7. Reserve `std::vector` capacity before push loops.
8. Debounce persistent writes. Do not write progress on every page turn.

## HAL And Platform Rules

- Use HAL classes, not SDK classes, in app code.
- File I/O uses `FsFile`, not Arduino `File`.
- Always close files explicitly.
- Use `MappedInputManager::Button::*` enums for button logic.

## C++ / Embedded Gotchas

- `string_view::data()` is not null-terminated. Do not pass it directly to C APIs.
- ISR handlers need `IRAM_ATTR`, and ISR-read data must be in DRAM, not flash-only storage.
- Never call `xSemaphoreTake()` from an ISR. Use ISR-safe give APIs.
- Do not cast unaligned `uint8_t*` data to wider pointer types. Use `memcpy`.
- No exceptions. No `abort()`. Log before returning failure.

## Activity Lifecycle

- Activities are heap-allocated and deleted on exit.
- Allocate long-lived buffers and tasks in `onEnter()`.
- Free resources in reverse order in `onExit()`.
- Delete FreeRTOS tasks before the activity is destroyed.
- Close open file handles in `onExit()`.
- Typical task stacks:
  - 2048 bytes for simple rendering work
  - 4096 bytes for network or EPUB parsing work

## Build And Verification

- PlatformIO is the source of truth. Personal overrides belong in `platformio.local.ini`.
- Logging uses `LOG_INF`, `LOG_DBG`, and `LOG_ERR`.
- The simulator env in this repo is `simulator`.
- For simulator work, build from this firmware repo unless the change belongs in `crosspoint-simulator` itself.

## Generated Files

- Do not edit generated files directly.
- HTML headers under `src/network/html/*.generated.h` come from `data/html/*.html` via `scripts/build_html.py`.
- I18n generated files under `lib/I18n/` come from `lib/I18n/translations/*.yaml` via `scripts/gen_i18n.py`.

## Cache Format

- EPUB cache lives under `.crosspoint/epub_<hash>/`.
- If you change binary cache layouts, bump the format version first and document it in `docs/file-formats.md`.
