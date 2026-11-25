---
title: Testing & Debugging
parent: Development
nav_order: 4
---

# Testing and Debugging

CrossInk runs on real hardware, so debugging usually combines local build checks, simulator checks, and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e simulator
pio run -e default
```

`pio run` without `-e` builds the X3/X4 and Sticky firmware targets from `platformio.ini`. Use it for a comprehensive build check, but prefer explicit environments while iterating.

## Flash and monitor

Flash firmware:

```sh
pio run -e default --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Touch keyboard diagnostic capture

On the keyboard diagnostic branch, closing a touch keyboard prints three `KBD`
summary lines. Counts and timings are collected in memory while typing; no typed
text, key values, coordinates, or passwords are logged by this diagnostic.

1. Build and flash the diagnostic revision, then connect the X4 Pro with a USB
   data cable. Leave USB Drive mode closed so the firmware serial port is available.
2. In a terminal in the firmware checkout, run `pio device list`. Find the device's
   USB serial port (on macOS, usually `/dev/cu.usbmodem...`).
3. Start capture, replacing `PORT` with that port:

   ```sh
   pio device monitor -e x4-pro --port PORT --baud 115200 --filter log2file
   ```

   The monitor prints the saved log path under `logs/device-monitor-*.log`.
4. Open an empty text keyboard, such as Library search. Tap `1234567890` twice
   with one thumb: 20 taps total. Wait a second and close the keyboard using the
   device's Home key. The three `KBD` lines appear when the keyboard exits.
5. Repeat once slowly and once at the speed that loses letters, using a fresh
   keyboard session each time. Record the displayed text for each run. Avoid
   corrections during the test so insertion counts can be compared with 20.
6. Press Ctrl+C to end capture. Send the saved log and the displayed text from
   both runs. No cache reset is required.

`contacts` and `multi` count observed contact sequences and transitions into
multiple contacts. `releases` counts the HAL's exposed release events;
`sdk_taps` and `mapped_taps` show taps accepted by each routing layer.
`actions` counts dispatched touch key actions, including controls;
`long_actions` is their long-press subset. `insertions` counts successful text
insert operations (a URL snippet is one operation).

`max_loop_gap_ms` measures the largest gap between keyboard loop calls;
`max_draw_ms` measures CPU drawing, and `max_display_ms` measures the display
submission/wait. These are observations from the firmware polling path, not a
trace of every controller frame: a contact never sampled by the firmware cannot
appear in the counters. Simulator compilation cannot verify hardware timing.

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing the affected book cache or using **Clear Reading Cache**

## Common troubleshooting references

- [Common Issues](../troubleshooting.md)
