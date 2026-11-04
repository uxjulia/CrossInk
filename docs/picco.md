# BOOX Picco

Picco support uses the existing FreeInk SDK profile, including SSD1677/SSD2677
panel detection, TMA525C touch, SDMMC storage, RTC, tilt, charging, and the
warm/cool frontlight. The release and debug environments are `picco` and `picco-debug` in `platformio.ini`.

In Controls, **Slide Switch** selects Frontlight, Reader Dark Mode, Toggle
Touchscreen, or Tilt Page Turn. Moving the switch in either direction toggles that function's
current state. Its physical position has no on/off meaning. Firmware controls
remain available; the next movement toggles the state they leave behind.

Startup, wake, restart, and changing the switch assignment do not trigger a
toggle. Moving the switch while the device is asleep is also ignored on wake.

Swipe down from the top edge to open the frontlight panel. In an open book,
swipe down anywhere for that panel, or swipe up for the reader menu. Use the
frontlight panel's Home button to leave the book. Outside the reader, the
bottom-edge upward swipe remains a Home shortcut.

The SDK queries the touch controller's configured contact limit at startup.
When it confirms two or more fingers, Controls exposes the existing two-finger
rotation, swipe, and pinch options. If the query fails or reports one contact,
single-finger touch remains available and multi-touch options stay hidden.
Startup logs report the contact limit or the capability-query failure.

The frontlight panel reads live touchscreen and light state, so slide-switch
changes update its icons and are preserved when the panel closes.

## Installation and updates

Build the `picco-debug` environment with the isolated ESP32-S3 toolchain.
The build uses `partitions_picco.csv` and the app-only upload helper. Serial
upload writes only `firmware.bin` to the stock application slot at `0xE0000`,
leaving the vendor bootloader, partition table, NVS, and recovery image intact.
Do not erase flash or install a merged factory image.

The stock layout has one application slot. CrossInk therefore hides its OTA
and SD firmware-update actions on Picco and disables its recovery-flashing
shortcut. Updates use the same external app-only installation path.

## Sleep and device checks

Sleep turns off the frontlight without clearing its saved preference. On USB
power, the device retains its sleep screen in light sleep. Power wakes it;
unplugging USB lets it enter deep sleep within about 30 seconds. There is no
live charging strip on the sleep screen. The normal Power-button interrupt is
masked before arming the USB sleep wake level, so holding Power during wake
cannot repeatedly trigger the normal input handler.

Frontlight on/off restores the saved brightness after allowing the I2C light
driver to leave standby. Moving the brightness slider is not needed to turn it
on after boot or sleep.

After installation, check:

- Open a book, turn pages with buttons and touch, and test touch orientation.
- Adjust brightness and warmth, then test each Slide Switch assignment.
- Change the assigned function in firmware with the switch stationary. Sleep,
  wake, and restart: the switch must not reapply its position. Existing light
  restore and schedule settings still apply. The next switch movement should
  invert the current state, in either direction.
- Sleep and wake on battery; check that the light turns off and restores.
- Sleep while charging, wake with Power, then sleep and unplug USB. Check the
  serial logs for charger setup and USB sleep, and measure idle current.
- Mount USB Drive, copy a book, eject, and confirm that reading resumes.
- Check battery reporting while charging and when full on a wall adapter.

No EPUB cache reset is required. Host tests and a successful build cannot verify
physical panel behavior, touch alignment, charger behavior, or sleep current.
