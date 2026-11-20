# Local X4 Pro firmware testing

CrossInk owns the build, flash composition, fixtures, assertions and evidence.
The emulator is an external dependency, initially pinned to
[`serialx/x4prosim` `55f6917885d89b60834cf8987177813e9fef648c`](https://github.com/serialx/x4prosim/tree/55f6917885d89b60834cf8987177813e9fef648c).
This supplements the fast native CMake/CTest, sanitizer and SDL simulator tests.
Keep those tests as the first checks for parser, store and resource changes.

## Build current changes and launch

On the configured Apple Silicon Mac:

```sh
crossink-emulator                 # X4 Pro
crossink-emulator x3
crossink-emulator x4
```

From any CrossInk checkout (including a nested directory), the command builds
that checkout, including staged, unstaged and untracked source and existing SDK
edits. Outside a checkout it defaults to `~/code/crossink`. Override with
`--source /path/to/crossink`, or set `CROSSINK_SOURCE` for a preferred checkout.
The launcher prints the selected source and build log before starting.
`crossink-emulator --list-devices` lists hardware-model availability.
`bin/crossink-emulator.command` also runs from Terminal or Finder; without the
installed short command, use `python3 scripts/x4pro_local/launch.py`.

Enable actual emulated Wi-Fi and optional device web-server access with:

```sh
python3 scripts/x4pro_local/launch.py --network --http-port 18080
```

This selects pinned eink-qemu 0.8.1 with its `esp32_wifi` adapter and user-mode NAT. Fresh
fixtures include the open `PICSimLabWifi` network; explicit `--sd` seeds keep
their own credentials. The firmware obtains `10.0.2.15` through DHCP and uses
its normal DNS, TCP and TLS stack. Host fixture services are reachable from
the device at `10.0.2.2`. Open File Transfer on the device to use
`http://127.0.0.1:18080`; forwarding binds only to localhost. Omit `--http-port`
for outbound access alone. Network mode is limited to native manual runs, so
the existing offline acceptance suite keeps its deterministic inputs. The run
manifest and launch log record the chosen network configuration. This exercises
firmware network flows, but does not model real radio reliability or throughput.
Use the checkout's Python entry point when an older installed short command
still points to another tooling checkout.

Do not enable networking on the offline 0.5.0 pin: its Wi-Fi receive and GDMA
models can overwrite guest memory. The network pin includes the upstream
[buffer and channel fixes](https://github.com/serialx/eink-qemu/commit/b05003dc3a0585c1422533d0aff3e2336fa34684).

The X4 Pro emulator can stop at the ROM boot log after a firmware
software restart (for example, entering or leaving File Transfer). A QMP
`system_reset` resumes boot while retaining the firmware's restart target.
This is an emulator limitation; record the reset in network validation results.

These are actual firmware emulators, with no SDL application-simulator fallback:

| Device | Firmware environment | Emulator machine |
| --- | --- | --- |
| X4 Pro | `x4-pro-debug` | `x4pro,panel=uc8179` |
| X3 | `debug` | `x3` |
| X4 | `debug` | `x4` |
| Sticky | Unavailable | No matching hardware model |
| X4 Classic | Unavailable | No matching hardware model |

The new launcher uses [eink-qemu 0.5.0](https://github.com/serialx/eink-qemu/releases/tag/0.5.0),
source `2c7443b56b7ddf10ffd8ab0124de6f4babdd1db7`. This is separate from the
older pinned browser and five-scenario acceptance workflows documented below.
The X4 Pro launcher selects the UC8179 panel modeled by the original workflow.
X3 and X4 share the C3 application environment, but run different hardware models.
Sticky and X4 Classic are rejected before building: sharing an ESP32 chip with
another board does not make its display, storage or input hardware compatible.

For X4 Pro tests that depend on activity-entry debug logs, set
`s3_debug_log_level = 2` under `[crossink]` in the source checkout’s
`platformio.local.ini` before building. The default S3 debug level is 1.

Each launch builds exactly one debug environment with `-j1`, using
`~/.local/bin/codex-pio-s3` for X4 Pro or `codex-pio-c3` for X3/X4. It builds in
the selected source checkout, with separate incremental build
outputs under `.pio/emulator-s3/build/` and `.pio/emulator-c3/build/`. This prevents
the two PlatformIO cores from clearing each other's build directories. Libraries
retain their standard per-environment `.pio/libdeps/` paths so the existing
SdFat and wolfSSL patch scripts continue to work. It
initializes missing submodules, preserving existing SDK checkouts and edits.
Do not run direct PlatformIO builds concurrently against the same MCU core;
the helper's MCU-specific lock serializes these launchers and the older helper.
The designated hardware build worktrees are not synchronized or reset.

A failed or changed-during-build source stops the launcher. A successful build
copies its three firmware binaries and source/hash record into a private build
directory **while still holding the build lock**. The emulator then receives
that copy explicitly, never the saved default firmware. It uses fresh flash and
a disposable SD card with a synthetic test EPUB; Wi-Fi is disabled by default. Runs and
build copies remain under `~/.local/state/crossink/x4pro-runs/` for inspection.
Each run retains roughly 80 MiB of flash/SD evidence plus build inputs. Old run
and build directories can be removed when no longer needed. Ctrl-C stops the
owned build/emulator processes. `--headless`, `--ready-timeout` and
`--overall-timeout` are available for diagnostics. Normal interactive runs last
at most one hour by default. Timeouts fail if Home and a completed panel refresh
are not observed; a visible window alone is not accepted as a successful boot.

The new runtime packages must be installed once. The launcher never downloads
or upgrades an emulator automatically. On this Mac they are already installed
under `~/.cache/crossink/eink-qemu/0.5.0/{xtensa,riscv32}/qemu/`. To reproduce,
download these assets from the release above, check the **archive** SHA-256,
and extract each archive's `qemu/` directory beneath its target directory:

| Apple Silicon archive | SHA-256 |
| --- | --- |
| `eink-qemu-xtensa-softmmu-0.5.0-aarch64-apple-darwin.tar.xz` | `ac6c03263be582205d9d900c6d53ff02e6ed3302bf5ab57a02551c906b328e7b` |
| `eink-qemu-riscv32-softmmu-0.5.0-aarch64-apple-darwin.tar.xz` | `b2b172dc52ff387e49fa75b99a263ee3273f8177901af4a0c190edc1fd5624af` |

Network runs use separate [0.8.1 release assets](https://github.com/serialx/eink-qemu/releases/tag/0.8.1),
installed under `~/.cache/crossink/eink-qemu/0.8.1/{xtensa,riscv32}/qemu/`:

| Apple Silicon archive | SHA-256 |
| --- | --- |
| `eink-qemu-xtensa-softmmu-0.8.1-aarch64-apple-darwin.tar.xz` | `8bfea8018492a68d7a57b08b5478b4baf2bcb43ba693179168ea26483c0292c3` |
| `eink-qemu-riscv32-softmmu-0.8.1-aarch64-apple-darwin.tar.xz` | `18a4740e51eb782a44aff7fb295a02c508ac2bc41b6b85da6cc2127d018ab125` |

The launcher additionally verifies each executable against its pinned digest in
`launch.py`. It requires the existing Python, mtools/dosfstools, Homebrew runtime
libraries, and MCU-specific PlatformIO wrappers described below. The short
`~/.local/bin/crossink-emulator` command points to the durable tooling checkout;
keep that checkout even if temporary implementation worktrees are removed.
This launcher neither flashes hardware nor proves hardware behavior. Compare
with a physical device by building the same source for that board, then checking
Home, opening the same book, and using its buttons/touch controls. Emulator
speed and memory behavior do not establish physical-device performance.

The launcher was validated on 2026-10-16 with current-source C3 `debug` and S3
`x4-pro-debug` builds at source `3fae9c15efcdba15ee13bda0cd4fc4cd619dee34`, SDK
`3af4166a36aa51a743e26a528d9c570106753a98`. X3, X4 and X4 Pro reached Home and
completed a panel refresh; the saved panel images were visually inspected.
The 22 host contract tests and independent code review passed. This verifies
build/launch behavior; the older five-scenario acceptance suite below was not
rerun or promoted to the newer runtime.

## Current acceptance status

On macOS arm64, the pinned native and browser runtimes boot CrossInk X4 Pro.
The native suite passes Home, synthetic EPUB opening/page-turn and a settings
change on source `6435493600de5e54a0ab823d884b4d76591bab75`, SDK
`fce2403b5b52042bf819e4b20b6bbcb16c9dfdaf`, using `x4-pro-debug`.
Installing the SD test language prepares generation 1, then the restarted
firmware asserts in ESP-IDF `spi_flash_munmap`. A fresh QEMU process with copies
of that flash/SD reproduces it. The decoded call chain is `I18n::begin()`
(`lib/I18n/I18n.cpp:103`) -> `HalFlashPartition::unmap()`
(`lib/HalFlashPartition/HalFlashPartition.cpp:137`) -> `esp_partition_munmap()`
-> `spi_flash_munmap()`. The diagnostic says the unmap pointer is outside
external memory regions. Firmware versus emulator cause is not established.

The five-scenario command intentionally fails on this combination. It does not
hide the crash or claim installation/restart acceptance. Resolve the mapping
compatibility failure before making this a CI gate. The complete suite passed
all five scenarios on an older local X4 Pro debug image, whose logged identity
was `1.6-next-development+8cd48`. Its exact build provenance is unavailable, so
that result validates the controls and assertions but does not validate this base.
Read-only comparison found no changes to the language mapping/unmapping code
between those versions. The newer base adds the PSRAM startup fallback in
`lib/hal/HalSystem.cpp:244`; this is an investigation lead, not an established
cause. A read-only GDB attempt could not inspect the live mapping because the
installed debugger and QEMU disagree on the remote register packet layout.

The reviewed language-download feature at
`65b26dc24e9392f81bc145490e9fd0a929b46ed7` was also built separately using its
existing `x4-pro-debug` configuration, without firmware source changes. Its
verified image passes the same first three scenarios, then asserts at
`I18n::begin():103` -> `HalFlashPartition::unmap():149` -> `spi_flash_munmap():248`.
The remaining two scenarios do not pass. Its existing production image visibly
boots Home but lacks debug activity logs; the compatibility preflight below
rejects that log profile before starting firmware. This is separate from the
observed mapping failure in the debug images.

## Trusted setup on this Mac

The durable tooling checkout is `/Users/home/code/crossink-x4pro-testing`.
Approved local artifacts are copied into the persistent, versioned cache
`/Users/home/.cache/crossink/x4prosim/55f6917885d89b60834cf8987177813e9fef648c/`:
native runtime in `native/qemu/`, browser archive in `browser/x4prosim-wasm.zip`,
and verified firmware inputs under `firmware/<source-commit>/<image-sha256>/`.
All 170 native bundle files and both archives were hash-checked after copying.
`copy-verification.json` retains those digests. Nothing was installed during
this durable setup, and existing `/tmp` evidence/diagnostic inputs were retained
at their original paths.

`defaults.json` inside that pinned cache selects the verified feature debug
image at source `65b26dc24e9392f81bc145490e9fd0a929b46ed7`, application SHA-256
`126e2564e249402a056feb1d34047ec39d4508b25ef85298ca873d8ce0f6817e`.
Its paths are relative to the cache and its emulator pin must match the runner.
The normal commands below need no temporary paths or additional flags.
`--runtime`, `--web-archive`, `--build-dir` and `--output` remain explicit overrides.
New runs default to `~/.local/state/crossink/x4pro-runs/`; private QMP sockets
remain temporary and are removed at teardown. The default image still has the
documented MMU blocker: setup readiness does not imply five-scenario acceptance.
`scripts/x4pro_local/provenance.json` records the full source pin, successful
GitHub Actions run `38066668497`, artifact IDs and SHA-256 digests:

| Package | Artifact ID | Archive SHA-256 |
| --- | --- | --- |
| macOS arm64 native | `11675089686` | `b8b009973b6e8476a236112838929b1e41ba4fc1ed2b47d211c54a5753b611e8` |
| browser WASM | `11675735170` | `6441c23cc4e6e4d66c27d5f271961a748b8a5656c66f7aee2d20a0661ca474f9` |

The native executable SHA-256 is
`84845734fae320f1324b6f0cc91c8c710b151eddc6a4b70c55304460a523aff4`.
The runner checks it before launch and checks the browser archive before
extracting. Download only the artifacts of the recorded successful run, verify
their digests, inspect the source and request any required execution approval
before running newly obtained code. The runner never downloads or installs.
Do not replace a denied installation with a bundled library, rebuild, trust
change, disabled certificate check or quarantine removal.

Required host tools are Python 3.9+, `mcopy`, `mtype`, `mkfs.fat`, and the approved
emulator. The native artifact links Homebrew GLib, Pixman, libpng, SDL2,
libslirp and libgcrypt. Existing trusted libraries were reused; the approved
additional installations were Homebrew core `libslirp 4.9.5`, `libgcrypt 1.12.4`,
`mtools 4.0.49`, `dosfstools 4.2`, and dependency `libgpg-error 1.61`.
Browser probing uses existing Node 26.11.1 (Node 22+ provides WebSocket) and
Chrome. Neither npm packages nor a new browser install are needed.

## Build and compose CrossInk firmware

Initialize an isolated firmware checkout with `git submodule update --init
--recursive`. Coordinate exclusive use of the S3 PlatformIO core before building.
Use the S3 wrapper; never run it concurrently with another direct S3 build.
The build helper locks cooperating calls, selects exactly one debug environment,
limits jobs to one, bounds build time and records the source, SDK and artifact
hashes. It fingerprints tracked/untracked firmware inputs, build scripts,
web/assets/board sources and ignored `platformio.local.ini` overrides, plus
the resolved PlatformIO configuration hash. The effective configuration is
hashed rather than copied into the manifest, so values in local overrides are
not disclosed. Source/configuration changes during the build reject attribution.
It does not change the source or flash hardware:

```sh
python3 /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local/build.py \
  --source /Users/home/code/crossink-x4pro-testing \
  --pio /Users/home/.local/bin/codex-pio-s3 \
  --log /Users/home/.local/state/crossink/x4pro-build.log
```

The designated `/Users/home/code/crossink-build-s3` checkout had unrelated
changes during initial setup. The initial build therefore used the isolated
checkout and the same trusted S3 wrapper/core, leaving that checkout untouched.
The helper is explicit about `--source`, so another isolated checkout can be
selected. Inspect cached dependencies first and obtain required approval before
letting PlatformIO install anything missing. Do not reuse the C3 package core.

Runs use the installed verified image by default. `--build-dir` can select a
separately built feature image, including an existing feature worktree; no merge or
edit is needed. Native manual readiness and all scenario predicates require
an `x4-pro-debug` build with the verbose local override (`LOG_LEVEL=2`),
because activity-entry messages are debug logs. The runner checks for their format-string signature
in the application before launching QEMU. An image without it exits `2`, with
manifest status `unsupported-image` and `firmware_started: false`; this is a
log-profile incompatibility, not a firmware crash or a failed Home boot.
Signature presence is a preflight check; the actual runtime predicates remain
unchanged and still must pass. Browser manual UI can use a production image,
but the optional CDP Home assertion also requires the debug messages.
A build record, when available, is included in the run manifest.
Without a record, hashes identify the input bytes but do not prove what source
produced them. A supplied record must contain matching SHA-256 values for all
three composed binaries; missing or stale hashes reject source attribution and
composition. `build_record_verification: artifact-hashes-match` verifies the
record's relationship to those bytes, not an independent signature of its
source claim. Keep the ELF beside the binaries for backtrace decoding.

The composer uses that build's `bootloader.bin` at `0`, `partitions.bin` at
`0x8000`, and `firmware.bin` at `0x10000`, leaving NVS and OTA metadata erased.
It checks ESP32-S3 headers, boot-asset bounds and app0 capacity, then creates a
complete 16 MiB image. Use X4 Pro firmware; a chip ID alone does not identify a
board or prove Sticky compatibility. Native readiness also checks the logged
`xteink_x4_pro` board. Browser receives the complete image, preserving CrossInk's
own bootloader and partitions rather than invoking CrossPoint bare-app composition.

## One manual command

```sh
python3 /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local/run.py manual
```

This opens native SDL with keyboard Up/Down/Power and pointer/touch controls.
Ctrl-C, SIGTERM and SIGHUP terminate the owned process group. Default duration is one hour;
`--overall-timeout` changes it. Native firmware logs and private QMP socket
details appear in the new run directory. Use structured JSON QMP `qom-set` for
`/machine/x4pro-keys` (`up`, `down`, `power` booleans), `/machine/gt911` (`tap`
string `X,Y` or `home`), `screendump`, `system_reset` and `quit`. The Python
`Native`/`QMP` classes expose these controls for custom assertion scripts;
do not use `drive.py` or shared `.run` images. QMP replies are matched by ID;
asynchronous reset/shutdown events are retained in the event log.

For browser manual use, change the backend of the same command:

```sh
python3 /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local/run.py manual --backend browser --cdp-port 0
```

The runner creates a fresh Chrome profile and an OS-assigned localhost HTTP port,
sets COOP/COEP, uses `wifi=off`, explicitly selects `turbo=0`, and serves only
local private copies. CDP is optional; `0` requests an automatic dedicated port.
The browser URL and profile are recorded in `browser.json`. For an optional
assertion, console capture and screenshot of this private tab:

```sh
node /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local/browser_probe.mjs /absolute/path/to/run-directory 90
```

The probe uses CrossInk Home entry followed by panel completion, independent of
the fork's CrossPoint `[MEM]` UI status. Download the SD image through the browser
before closing if guest writes are wanted as an artifact. Browser SD is profile
persistent; flash writes are ephemeral. Editing card files resets the guest.
Installation and flash-persistence assertions therefore run natively.
Timing is the emulator's model timing, not a measured hardware performance result.

The WASM HTTP adapter accepts untrusted guest TLS and strips Authorization.
Keep browser runs offline here. Use native QEMU's opt-in Wi-Fi/NAT for HTTPS
tests; it retains CrossInk certificate validation. The default offline workflow
uses `-nic none` and adds no port forwards. Neither mode changes certificate,
browser security or system permissions. Never upload private firmware to the
hosted emulator.

## One agent test command

```sh
python3 /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local/run.py test
```

The suite uses a fresh `sd-assets` card, Lyra portrait layout, Small UI scale,
English fallback labels and default text antialiasing. It asserts:

1. CrossInk X4 Pro Home entry and a completed panel refresh.
2. Open original synthetic `TestBook.epub`, render a 32-page chapter, turn one
   page, leave the reader and verify saved spine `0`, page `1`, and page count.
3. Change Display > Dark Mode and verify `screenInverted: 1` in saved SD JSON.
4. Install `Local Test` language from SD and verify `ZZ-TEST`, generation `1`
   after restart; install the French hyphenation pack and verify one mapped pack.
5. Reset the machine, verify settings/language/pack and book progress persist,
   then reopen the book from its cache and recheck progress.

Default readiness/action/overall timeouts are 90/30/600 seconds. Override with
`--ready-timeout`, `--action-timeout`, `--overall-timeout`. Navigation waits on
activity and panel logs. EPUB rendering waits through grayscale completion and
framebuffer restoration. Small key-hold intervals model input; arbitrary boot
sleeps are not success predicates. Firmware panic/assert/watchdog signatures,
unexpected process exit and unmet assertions fail the command. A crash is
detected before readiness is accepted. A failed run keeps the final panel and
all available evidence and terminates the process; it never marks unexecuted
scenarios passed. Tests do not rewrite or preinstall the expected settings.

Named disposable fixtures are `blank-home`, `reading`, and `sd-assets`. The
synthetic EPUB text is CC0; French patterns come from CrossInk's maintained
snapshot and retain its notices in `assets/hyphenation`. No private books or
credentials are used. Fixture timestamps and FAT metadata are fixed, so repeated
generation with the recorded host tools produces the same seed bytes. Manual
`--sd /path/to/seed.img` copies an explicit seed. The five-scenario suite requires
its known `sd-assets` fixture. To replay a retained seed, add
`--sd /path/to/run/fixture/sd.img`; its adjacent `fixture.json` must name
`sd-assets` and match the unchanged seed hash. Do not use a guest-modified card
for fresh-fixture acceptance.

Each run prints a new private directory under `~/.local/state/crossink/x4pro-runs`:
`manifest.json` contains input hashes, source/build references, pin, timeouts,
results and scenario status; `events.jsonl` holds QMP requests/replies;
`serial.log`, `stdout.log`, `stderr.log`, panel PNGs, final `flash.bin` and
`sd.img` remain. Native teardown extracts `.crosspoint` into `sd-results` and
records extraction status. The input SD seed is rehashed after the run.
Short private Unix socket paths avoid macOS path limits. No shared flash, card,
HTTP port or browser profile is reused. The owned process group is terminated,
waited for and killed if necessary; socket directories are removed. Final panel
capture and graceful QMP shutdown have a separate five-second cleanup budget
after an expired run deadline. SIGKILL or host failure cannot run cleanup;
use the recorded PID/process group for recovery in those cases.
Teardown checks the owned process group independently of its leader and kills
surviving descendants, including a child that ignores SIGTERM after the leader
exits. The same bounded group cleanup is shared by QEMU, Chrome and build runs.

Host runner contracts can be checked without firmware:

```sh
python3 -m unittest discover -s /Users/home/code/crossink-x4pro-testing/scripts/x4pro_local -p 'test_*.py' -v
```

These tests include a repository-owned stub QMP process to check SIGTERM,
SIGHUP and deadline cleanup, so sandboxed execution may require approval to
bind its temporary local Unix socket.

For hardware comparison, on a separately authorized X4 Pro use the same fresh
SD fixture files: open/turn/close TestBook, toggle Dark Mode, install Local Test
under System > Device > Language and French under Reader > Page Layout >
Hyphenation Packs, then restart and reopen. Expect page two, saved Dark Mode,
`Mapped ZZ-TEST` and `Mapped 1 packs`, with no MMU/assert diagnostic. Fresh cards
need no cache reset; if reusing a card, clear only this book's `epub_<path-hash>`
cache when validating layout changes. Hardware flashing is outside this workflow.

## Plugin networking regression fixture

`scripts/x4pro_local/plugin_fixture.py` creates a fresh SD seed and serves a
synthetic AES-256 EPUB, catalog details, and event acknowledgments over host NAT.
It needs the same FAT tools as the runner plus `openssl`. Its public test key
must never be used for real books. The server binds only to localhost and logs
requests and response statuses in `requests.jsonl` under the chosen directory.

Start the service in one terminal, using a new output directory:

```sh
python3 scripts/x4pro_local/plugin_fixture.py --output /tmp/crossink-plugin-fixture
```

In another terminal, launch a freshly built `x4-pro-debug` image from the branch
being tested (replace `/path/to/verified-build` with the build helper's immutable
output directory):

```sh
python3 scripts/x4pro_local/run.py manual --release-runtime --network \
  --http-port 18080 --build-dir /path/to/verified-build \
  --sd /tmp/crossink-plugin-fixture/fixture/sd.img
```

Use the actual firmware UI to check:

1. On first boot, dismiss What's New and wait at least 30 seconds before joining
   Wi-Fi. This exercises background plugin time sync alongside first-time RTC
   sync; neither may assert. Open Plugins, download Plugin Hub, and install Hello Plugin from the live hub.
   Confirm its files persist under `/.crosspoint/plugins/hello`.
2. Open Network test and select Network AES256. Details must show “Network catalog
   details”; `/wrong-details` must never be requested. A filtered duplicate title
   with a different ID/version precedes the selected item, and the discarded
   metadata deliberately exceeds the former details capture budget.
3. Download the book. Compare its SD bytes with `protected.epub` using the hash in
   `plugin-fixture.json`. Before provisioning a key, opening it must be rejected.
4. Enter File Transfer's Wi-Fi web server and provision the synthetic key:

   ```sh
   curl --fail --json @/tmp/crossink-plugin-fixture/key-request.json \
     http://127.0.0.1:18080/api/book-key
   ```

   The API must succeed and create a 74-byte wrapped `.key` beside the book.
   Exit File Transfer, open the book, turn a page, read for at least 15 seconds,
   and return Home. The encrypted text must render normally.
5. Allow network delivery, then sleep. Verify `book.downloaded`, `reader.open`,
   `reader.session`, `reader.exit`, and `sleep.enter` requests. The service returns
   503 on its first event request: the same event ID must retry and receive 200,
   and the acknowledged queue must drain.

Use the existing QMP controls for screenshots and read-only SD inspection;
never edit or mount the guest's live SD image. If the emulator stalls at ROM
entry after a firmware-requested restart, record an explicit QMP `system_reset`
before continuing; this is an emulator workaround, not a passing restart check.
This fixture tests synthetic protected content, not a real provider's account or
license service. Actual emulation does not validate physical Wi-Fi or SD timing.
