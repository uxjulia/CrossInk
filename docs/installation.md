---
title: Installation
nav_order: 2
---

# Installation

## Supported Devices

- Xteink X3 and X4
- Xteink X4 Classic and X4 Pro
- Seeed Studio Sticky

Don't have a device yet? Get one directly from [Xteink](https://go.sjv.io/X4RGBb) or [Seeed Studio](https://www.seeedstudio.com/reTerminal-Sticky-p-6861.html?sensecap_affiliate=1Nxo3Gw&referring_service=link).

Note: Your purchases using the above affiliate links help support ongoing development of Crossink.

## Languages and hyphenation

You can add menu languages and hyphenation packs without erasing your reader.
Some devices keep installed packs on the SD card, so keep that card in the
reader. See [Languages](./languages.md) for setup and recovery.

## Web Installation via USB

#### For new installs and updates.

Flashing means writing the CrossInk firmware (the reader's software) onto the device. Inky does this from your browser over a USB cable. Use a browser that supports it, such as Chrome or Edge.

1. Connect your reader to your computer with a USB-C cable.
2. Open [https://inky.crossink.dev/#flash-tools](https://inky.crossink.dev/#flash-tools) and select your device model.
3. The latest version is selected for you. To go back to an earlier build, pick it from the dropdown.
4. Choose the firmware option you want to install.
5. Click **Flash Firmware** and follow the prompts.

Keep the reader connected until Inky says it is finished. Some devices need to be put into download mode first. If Inky asks, follow the steps it shows. Always select your exact device model.

## USB Drive

On the X4 Classic and X4 Pro, you can plug the reader into your computer and use its SD card like a USB stick. This is handy for copying books and fonts.

1. On the reader, go to **Home > File Transfer > USB Drive**.
2. Connect the reader to your computer with a USB-C cable.
3. When you are done, eject the drive on your computer first, then unplug the cable.

The reader returns to the Home screen once the drive is ejected or the cable is removed.

## SD Card Firmware Update

#### For installing newer versions of CrossInk. Can be used by USB locked devices.

1. Follow the steps from the Web Installation method above. Inky has an option to download the firmware instead of flashing over USB.
2. Copy the downloaded `firmware-*.bin` file to your SD card. It can go in any folder.
3. On the reader, go to **Settings > System > SD Card Firmware Update**, choose the `.bin` file, and confirm the update.
4. Do not turn the reader off until the update finishes.

If your reader already runs CrossInk, **Settings > System > Check for Updates** can also check for the latest version and install it over Wi-Fi.

## USB Locked Devices

Some devices ship with USB data transfer turned off, so a computer cannot flash them. If that is yours:

1. Open [https://inky.crossink.dev/#flash-tools](https://inky.crossink.dev/#flash-tools) and tick **I have a locked device** at the top.
2. The latest version is selected for you. To go back to an earlier build, pick it from the dropdown.
3. Choose the firmware option you want to download.
4. Click **Download update.bin** and follow the instructions on the page.
