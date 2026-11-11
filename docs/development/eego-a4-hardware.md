---
title: EEGO A4 Hardware and Validation
parent: Development
---

# EEGO Reader A4

CrossInk uses the existing FreeInk SDK EEGO A4 profile: ESP32-S3 N16R8
(16 MB DIO flash and 8 MB OPI PSRAM), 768 × 552 UC8279C display, GSLX680
touch, two page buttons, Power, PCF8563 clock, and a dedicated HSPI SD card.
The SDK probes the optional LM3630A frontlight and CW2015 battery gauge;
units without the gauge use ADC battery readings. No GPIO or touch calibration
is duplicated in CrossInk. The framebuffer lives in PSRAM.

## Build and flash

Initialize dependencies before the first build:

```sh
git submodule update --init --recursive
```

Choose one environment for each build:

```sh
# Normal firmware (info logs)
pio run -e eego_a4 -j1
# Development firmware (debug logs and build details)
pio run -e eego_a4-debug -j1
# Flash the chosen image, then monitor its serial port
pio run -e eego_a4-debug -j1 --target upload
pio device monitor -e eego_a4-debug --baud 115200
```

Both environments use the prebuilt `dio_opi` Arduino core so USB Drive can
switch from USB Serial/JTAG to TinyUSB MSC. The SD card uses SPI, not SDMMC;
`USE_BLOCK_DEVICE_INTERFACE` enables the SDK's SPI card handoff for USB Drive.
Both use the standard CrossInk dual-app `partitions.csv` layout and support OTA.
Back up stock firmware and the SD card before first installation; CrossInk's
partition layout may differ from the stock firmware.

The local artifact is `firmware-eego_a4.bin` under the selected `.pio/build/`
environment. Stable builds publish `firmware-eego_a4-v<version>.bin`; RC builds
publish `firmware-eego_a4-<branch>-<hash>-RC.bin`. Stable release catalogs use
variant `eego_a4` and supported device `eego_a4`, matching the SDK profile and
the embedded board tag used to reject images for a different board.

## Hardware verification

Compilation and host checks do not validate the physical device. On an A4:

1. Flash the debug environment. Confirm serial logs identify `eego_a4` and the
   UC8279C display, and the reader stays awake after flashing or USB connection.
2. Open a book from SD. Check all four orientations, bezel margins, page
   buttons, Power, touch taps/swipes, menus, and text selection.
3. Compare B&W pages, text anti-aliasing, gray images, and sleep covers. Check
   repeated page turns and full refreshes for ghosting or missing content.
4. Save a screenshot. Confirm the BMP is 552 × 768 and opens correctly.
5. On a lit unit, check brightness/warmth and that the light turns off for
   sleep. On an unlit unit, confirm frontlight controls are hidden.
6. Test sleep and wake on battery and USB power. Touch must reinitialize after
   wake; verify the battery percentage and charging indicator on both gauge
   and ADC variants where available.
7. Open File Transfer → USB Drive, read/write a test file from the computer,
   safely eject it, and exit. Confirm SD access and USB serial recover.
8. Check OTA selects the A4 asset and rejects another board's tagged image;
   verify the installed version in Settings → System → About.

No cache-format change is included; existing book caches need no forced reset.
See the pinned SDK's `freeink-sdk/docs/eego-a4-support.md`
for driver provenance and remaining hardware limitations.
