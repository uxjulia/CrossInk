# CrossInk — Shared Agent Guide

This is the canonical repo instruction file.
`CLAUDE.md` should point here so Codex and Claude read the same guidance.

Project: Open-source e-reader firmware for ESP32-C3 and ESP32-S3 devices.

## Architecture And Fast Navigation

Start narrow. Read the named entry point and its direct collaborators before
scanning a whole subsystem; `rg` exact symbols and `rg --files <directory>`
are the default discovery tools. The repository is intentionally split between
application policy here and reusable hardware/UI implementation in the nested
SDK.

| Area | Read first | Owns |
| --- | --- | --- |
| Boot/runtime | `src/main.cpp`, `src/CrossPointSettings.h`, `src/CrossPointState.h` | Hardware startup, app singletons, global input/power routing, resume and sleep policy. |
| Screen flow | `src/activities/Activity.h`, `src/activities/ActivityManager.{h,cpp}` | The activity stack, lifecycle, result callbacks, render scheduling, and top-level navigation factories. |
| Reading | `src/activities/reader/ReaderActivity.h`, the matching `EpubReaderActivity`, `TxtReaderActivity`, or `XtcReaderActivity` | Reader UI, controls, progress, dictionary/clipping flows, and format dispatch. |
| EPUB engine | `lib/Epub/Epub.{h,cpp}`, `lib/Epub/Epub/` | ZIP/OPF/HTML/CSS parsing, layout, page model, hyphenation, images, and SD cache serialization. |
| UI and input | `src/components/UITheme.{h,cpp}`, `src/MappedInputManager.{h,cpp}`, `src/QuickActions.{h,cpp}` | App theme policy, logical buttons/gestures, shortcuts, and app-specific touch components. |
| Hardware boundary | `lib/hal/`, `include/AppCapabilities.h`, `include/DeviceCapabilities.h`, then `freeink-sdk/` | CrossInk HAL wrappers and capability gating; display, storage, input, power, and board drivers live in the SDK. |
| Network and transfers | `src/activities/network/`, `src/network/CrossPointWebServer.{h,cpp}` | Wi-Fi flow, web/WebDAV/WebSocket transfer, OTA, Calibre, Nearby, and USB Drive activities. |
| Persistence | `lib/Serialization/`, `src/*Store.*`, `src/clippings/`, `docs/data-cache.md` | Settings/session data plus per-feature SD stores; EPUB cache is a separate layout/cache concern. |

### Runtime Boundaries

- `main.cpp` owns the global `renderer`, `mappedInputManager`, and `activityManager`; it initializes the device and calls the activity loop.
- `ActivityManager` owns activities with `std::unique_ptr`, applies push/pop/replace changes on the main loop, and renders on a dedicated FreeRTOS task. Do not mutate activity stack or renderer state from arbitrary tasks; use its request/navigation APIs and `RenderLock` contract.
- Activities are foreground-only screen controllers. Put durable setup in `onEnter()`, release resources in `onExit()`, and use `startActivityForResult()`/`setResult()` for nested flows rather than hand-rolled global state.
- The app uses `SETTINGS` for persisted preferences and `APP_STATE` for persisted session/runtime context. Before adding another store or global, search these and the existing `*Store` classes.
- Readers own reader interaction and persistence coordination; `lib/Epub` owns content parsing/layout/cache data. Keep UI policy out of the EPUB library and reusable device behavior out of app activities.
- App code should use `lib/hal` and FreeInkUI-facing abstractions. Change `freeink-sdk` only when behavior is broadly hardware/UI reusable; a parent-repo change must also advance the submodule gitlink to integrate an SDK fix.

### Directory And Generated-Asset Map

- `src/activities/{boot_sleep,home,reader,settings,network,util}`: screen controllers grouped by user flow.
- `src/components/`: shared app UI, themes, touch helpers, and generated icon headers. `src/util/` holds app-domain helpers such as dictionaries, cache utilities, navigation, and string/layout helpers.
- `lib/EpdFont`, `lib/GfxRenderer`, `lib/FsHelpers`, `lib/Memory`, `lib/I18n`, `lib/Serialization`, `lib/Txt`, and `lib/Xtc` are focused local libraries. Prefer extending the closest existing library over adding a cross-cutting helper.
- `freeink-sdk/` is a submodule providing the FreeInkUI, HAL-backed device drivers, board profiles, and network primitives. Check its pinned SHA with `git submodule status` before diagnosing or claiming an SDK integration.
- `test/` is a native CMake/CTest suite with isolated target folders; `test/epubs-src/` contains fixture sources and `test/device/` contains device-oriented checks. `scripts/run_simulator_smoke_test.py` is the broad app-flow regression tripwire.
- `web/templates/`, `web/pages/`, and `web/assets/` are the editable web portal sources. `site/` is repository website content, not firmware UI.
- Do not edit generated outputs: `src/network/html/*.generated.h` (from `scripts/build_web.py`), `lib/I18n/I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp` (from `scripts/gen_i18n.py`), icon headers listed by `src/components/icons/*.manifest` (from `scripts/generate_icons.py`), or EPUB hyphenation tries under `lib/Epub/Epub/hyphenation/generated/`.

### Target Selection

- `default`: Xteink X3/X4, ESP32-C3, buttons, SPI SD, no touch/PSRAM/USB Drive.
- `sticky`: reTerminal Sticky, ESP32-S3, touch, SPI SD, PSRAM framebuffer.
- `x4-pro`: Xteink X4 Pro, ESP32-S3, touch, SDMMC, PSRAM framebuffer, and USB Drive capability.
- `simulator`, `simulator-X3`, `sticky-simulator`, and `x4-pro-simulator`: native profiles. Match the simulator profile to the capability/device branch being changed.
- `platformio.ini` is the capability-source build matrix. Read the matching environment's flags before adding `#if` branches; use `AppCapabilities`/device capability helpers rather than duplicating macro checks in activities.

## Core Rules

- Role: Senior Embedded Systems Engineer for ESP-IDF / Arduino-ESP32 work.
- Support both constrained ESP32-C3 devices and PSRAM-equipped ESP32-S3 devices. Keep shared code safe for the C3 unless it is explicitly capability-gated; stability beats features.
- Cite file paths and line numbers before proposing non-trivial changes.
- Do not assume ESP-IDF or SDK API availability. Verify in `freeink-sdk/` or the live code.
- Do not claim performance or memory wins without explaining the mechanism, such as reduced heap churn, flash vs DRAM placement, or smaller stack use.
- Justify new heap allocations or explain why stack/static storage is not suitable.
- Explain fixes in plain language where possible, ideally in terms a Node / React developer would follow.
- After proposing or making a fix, say how to verify it on hardware.

## Persistent Context

- Read `.claude/CONTEXT.md` at session start for durable repo-specific gotchas.
- Keep `.claude/CONTEXT.md` short. Add only reusable findings, not turn-by-turn history.

## Repo Skills

- Do not read every `.claude/skills/*/SKILL.md` at session start.
- Use this section as an index. Read a local skill only when the task clearly matches its folder name or purpose.
- Current local skills:
  - `control-flow-clarity`: simplify confusing logic without behavior changes.
  - `refactor-for-review`: small refactors intended to reduce review risk.
  - `hal-and-abstractions`: HAL boundaries and platform abstraction work.
  - `heap-discipline`: memory allocation, lifetime, and fragmentation-sensitive work.
  - `scope-discipline`: keep changes narrow and avoid unrelated cleanup.
  - `custom-fonts`: font generation, conversion, and SD/built-in font work.
- Treat these as task-specific playbooks layered on top of this guide. If a skill conflicts with this file, prefer `AGENTS.md` and note the conflict.

## Hardware Constraints

- ESP32-C3 targets (Xteink X3/X4): single-core RISC-V at 160 MHz, no PSRAM, and about 380 KB usable internal RAM.
- ESP32-S3R8 targets (Seeed reTerminal Sticky/Xteink X4 Pro): dual-core Xtensa at up to 240 MHz with 8 MB PSRAM. PSRAM is slower than internal DRAM and is not suitable for every DMA, ISR, or latency-sensitive buffer.
- Current displays use an 800x480 1-bit e-ink framebuffer: `800 * 480 / 8 = 48000` bytes. Use runtime renderer dimensions because orientation and future device profiles may differ.
- Use one framebuffer only. C3 targets keep it in internal RAM; current S3 targets place it in PSRAM via `FREEINK_FB_PSRAM`.
- Storage is exposed through SdFat, but the transport is device-specific (SPI SD on X3/X4/Sticky and SDMMC on X4 Pro). On real hardware, only one reader can hold a file open at a time.

## Resource Rules

1. Keep local stack usage small. Anything meaningfully larger than 256 bytes should be justified.
2. Avoid repeated heap churn in loops. Allocate once in `onEnter()`, reuse, and free in `onExit()`.
3. Large constant tables should be `static const` so they live in flash, not DRAM.
4. Avoid `std::string` and Arduino `String` in hot paths. Prefer `string_view`, `char[]`, and `snprintf`.
5. All user-facing UI strings must use `tr(STR_*)`. Logs may be hardcoded.
6. Prefer `constexpr` for compile-time constants.
7. Reserve `std::vector` capacity before push loops.
8. Debounce persistent writes. Do not write progress on every page turn.
9. `new` is not nothrow on ESP32. With exceptions disabled, bare `new` calls `abort()` on allocation failure instead of returning `nullptr`. Use `new (std::nothrow)` or `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h` for fallible allocations.
10. Prefer `makeUniqueNoThrow<T>()` / `makeUniqueNoThrow<T[]>()` for owned heap allocations so cleanup is automatic on early returns.
11. Use raw `malloc` or `new (std::nothrow)` only when a C or SDK API takes ownership; add a short comment explaining that ownership transfer.
12. Treat PSRAM as a device capability, not a universal assumption. Keep shared paths within C3 limits or gate S3-only allocations behind the relevant board/capability macro, and handle PSRAM allocation failure.

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
- Avoid `std::function` in hot paths and library code; prefer function pointers or a small context/callback struct.
- Keep template use deliberate. If a template is needed in shared code, consider explicit instantiation in a `.cpp` file to avoid repeated binary growth.

## Error Handling

- Prefer `LOG_ERR(...)` plus `return false` for recoverable failures.
- Prefer `LOG_ERR(...)` plus a known fallback when the app can continue safely.
- Use `assert(false)` only for truly impossible fatal states.
- Use `ESP.restart()` only for intentional recovery flows, such as completing OTA.
- Always log before returning failure from allocation, file, parse, network, or hardware paths.

## Activity Lifecycle

- Activities are heap-allocated but owned by `ActivityManager` `std::unique_ptr`s; do not manually delete them.
- Allocate long-lived buffers and tasks in `onEnter()`.
- Free resources in reverse order in `onExit()`.
- Delete FreeRTOS tasks before the activity is destroyed.
- Close open file handles in `onExit()`.
- Typical task stacks:
  - 2048 bytes for simple rendering work
  - 4096 bytes for network or EPUB parsing work

## UI And Input

- Do not hardcode screen dimensions like `800` or `480`; use renderer dimensions and orientation helpers.
- Use `renderer.getOrientedViewableTRBL()` for layout that must stay inside usable bezel-safe bounds.
- Use logical `MappedInputManager::Button::*` values in activities; raw hardware button indices belong only in button-mapping code.
- Route UI drawing through `UITheme` / `GUI` where practical so fonts, spacing, and orientation behavior stay consistent.
- User-facing text must be translated with `tr(STR_*)`; logs can remain hardcoded.

## Build And Verification

- PlatformIO is the source of truth. Personal overrides belong in `platformio.local.ini`.
- Host environment may be macOS, Linux, WSL, or Windows Git Bash. Check `uname -s` before recommending platform-specific shell commands.
- Logging uses `LOG_INF`, `LOG_DBG`, and `LOG_ERR`.
- The simulator env in this repo is `simulator`.
- For simulator work, build from this firmware repo unless the change belongs in `crossink-simulator` itself.
- Common validation commands:
  - `pio run -e simulator` for simulator-facing UI/reader work.
  - `pio run -e default` for the ESP32-C3 X3/X4 firmware.
  - `pio run -e sticky` for the ESP32-S3 Sticky firmware.
  - `pio run -e x4-pro` for the ESP32-S3 X4 Pro firmware.
  - `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high` for static analysis.
  - `find src lib include test -name "*.cpp" -o -name "*.h" | xargs clang-format -i` for formatting touched C++ files.
- For crash debugging, check serial logs, internal heap with `ESP.getFreeHeap()` and `ESP.getMaxAllocHeap()`, task stack high-water marks, and whether cache files need clearing. On S3 targets, also inspect PSRAM free space and largest allocatable block; abundant PSRAM does not prove that internal-RAM or DMA-capable allocations can succeed.
- Hardware verification should mention the concrete device path to test, expected UI/log behavior, and any cache reset needed.

## Generated Files

- Do not edit generated files directly.
- Web portal headers under `src/network/html/*.generated.h` are built by `scripts/build_web.py` from sources in `web/`: pages compose `web/templates/base.html` (shared chrome) with `web/pages/<slug>.{html,css,js}`, plus shared assets `web/assets/style.css` (served at `/style.css`) and `web/assets/logo.png` (served at `/logo.png`). Edit the `web/` sources, never the generated headers.
- I18n generated files under `lib/I18n/` come from `lib/I18n/translations/english.yaml` and `lib/I18n/languages.json` via `scripts/gen_i18n.py`. Other YAML files are community SD-language assets, not firmware build inputs. New English keys do not require community translations; use a `_FORMAT` suffix for new printf-format keys. See `docs/languages.md`.
- Icon headers are generated from the manifests in `src/components/icons/` with `scripts/generate_icons.py`; hyphenation trie headers are generated under `lib/Epub/Epub/hyphenation/generated/`. Edit their source manifests/data and regenerate.

## Cache Format

- EPUB cache lives under `.crosspoint/epub_<hash>/`.
- If you change binary cache layouts, bump the format version first and document it in `docs/file-formats.md`.
- Cache identity is tied to the book path hash; moving or renaming a book creates a different cache.
- Clear the relevant `.crosspoint/epub_<hash>/` cache when testing EPUB parser, layout, image, or binary cache format changes that may otherwise reuse stale output.

## Git Workflow

- Check `git status --short` before edits and before reporting results. Preserve unrelated user changes.
- When resolving merge, rebase, or cherry-pick conflicts, inspect the relevant commit messages for upstream PR references such as `#2608`. Open the PR in its source repository and read its description and changed files before resolving the conflict so the intended behavior is understood.
- Do not resolve conflicts by automatically keeping CrossInk's current implementation or by discarding the upstream change wholesale. Preserve or adapt the upstream intent unless it is already fully implemented, would introduce a regression, or would substantially and unjustifiably change CrossInk's UX or behavior. When rejecting an upstream change, state the concrete reason.
- If a referenced PR cannot be accessed, inspect the source commit diff and nearby history, then report that the PR intent could not be verified instead of guessing.
- Do not commit unless the user explicitly asks or committing is part of the skill utilized.
- Before staging, ensure ignored/generated/local files such as `.pio/`, `*.generated.h`, `compile_commands.json`, and `platformio.local.ini` are not included.
- Branch names should use repo-style prefixes such as `feat/`, `fix/`, `docs/`, `refactor/`, `test/`, or `chore/`.
- Suggested commit messages should follow `<type>: <short summary>`, using types like `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, or `perf`.

## Changelog

When new features are added or issues are fixed, make sure to add an entry to `CHANGELOG.md` with the user-facing description of the change. Types of changes should have their own section.

### Changelog Guiding Principles

- Changelogs are _for humans_, not machines.
- There should be an entry for every single version.
- The same types of changes should be grouped.
- Versions and sections should be linkable.
- The latest version comes first.
- The release date of each version is displayed.

### Types of Changelog Changes

- Added - for new features.
- Changed - for changes in existing functionality.
- Deprecated - for soon-to-be removed features.
- Removed - for now removed features.
- Fixed - for any bug fixes.
- Security - in case of vulnerabilities.
