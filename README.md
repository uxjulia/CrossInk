> **This is a personal fork of CrossPoint Reader** with a focus on improved fonts and minimal reading stats.

## What's different in this fork

My goal with this fork was to maintain the core Crosspoint firmware while integrating my preferred typography and some lightweight reading statistics. I’ve focused on keeping the underlying system stable while layering in a few 'nice-to-have' features and UI refinements along the way.

### Summary

- New reader fonts: ChareInk, Lexend Deca, and Bitter
- Unicode emoji and miscellaneous symbols support
- Adjusted font sizes: Tiny (10pt), Small (12pt), Medium (14pt), Large (16pt)
- In-book menu to quickly adjust font options without having to exit the book
- Reading stats per book (total reading time, number of sessions, pages turned, average session time)
- Device simulator during development

---

### Reader Fonts

The default fonts have been replaced with Bitter, Lexend Deca, and Atkinson Hyperlegible. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [ChareInk](https://www.mobileread.com/forums/showthread.php?t=184056) - A cult favorite among the e-reading community for over a decade based off of the typeface [Charis](https://software.sil.org/charis/). It is specially designed to make long texts pleasant and easy to read.
- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) — A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) — A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4.

The UI now uses [DM Sans](https://fonts.google.com/specimen/DM+Sans) as the display font and the smallest text on the UI has been replaced with [Inter](https://fonts.google.com/specimen/Inter), both fonts have improved readability at smaller sizes.

### Emojis and Misc Glyphs

- Support for Unicode [Emoticons](https://unicode-explorer.com/b/1F600) and [Miscellaenous Symbols](https://unicode-explorer.com/b/2600) using [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji) and [Noto Sans Symbols](https://fonts.google.com/noto/specimen/Noto+Sans+Symbols) font.

### Font Sizes

I've removed the "Large" (18pt) font size in favor of a "Tiny" (10pt) font size. The new font size definitions are as follows:

- Tiny (10pt)
- Small (12pt)
- Medium (14pt)
- Large (16pt)

### Reader options in the in-book menu

Reader settings (font, size, line spacing, margins, alignment, etc.) are now accessible directly from the in-book menu without leaving the book. Open the menu while reading and select **Reader Options** to adjust any reader setting on the spot. Changes take effect immediately.

### Reading stats

Some simple per-book reading stats are tracked automatically and displayed in two places:

**In-book menu → Reading Stats:**

- Total reading time
- Number of sessions
- Pages turned
- Average session time

**Home screen book card (Lyra theme only):**

- Total reading time
- Average session time

### Development Device Simulator

A [device simulator](https://github.com/uxjulia/crosspoint-simulator) has been added for development purposes to quickly sanity check updates without having to flash the firmware every time. It renders the e-ink display in an SDL2 window. Use with Platformio by choosing the `simulator` environment.

> **Platform support:** The simulator is currently configured for **macOS (Apple Silicon)** only. The `platformio.ini` `[env:simulator]` section contains hardcoded `-arch arm64` and Homebrew paths (`/opt/homebrew`). Intel Mac users need to remove `-arch arm64` and change those paths to `/usr/local`. Linux requires the same path changes plus a replacement for `lib/simulator_mock/src/MD5Builder.h` (which uses the macOS-only `CommonCrypto` API). Native Windows is not supported; use WSL and follow the Linux instructions.

**Prerequisites:** SDL2 must be installed.

```bash
# macOS
brew install sdl2

# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev
```

**Setup:** Place EPUB books in `./fs_/books/` relative to the project root (this maps to the SD card `/books/` path on device).

**Build and run:**

```bash
pio run -e simulator
.pio/build/simulator/program
```

**Keyboard controls:**

| Key    | Action                             |
| ------ | ---------------------------------- |
| ↑ / ↓  | Page back / forward (side buttons) |
| ← / →  | Left / right front buttons         |
| Return | Confirm / Select                   |
| Escape | Back                               |
| P      | Power                              |

> **Note:** On first open of an ebook, an "Indexing..." popup will appear while the section cache is built in `.crosspoint/`. If you see rendering issues after a code change, delete `./fs_/.crosspoint/` to clear stale caches.

---

# CrossPoint Reader

Firmware for the **Xteink X4** e-paper display reader (unaffiliated with Xteink).
Built using **PlatformIO** and targeting the **ESP32-C3** microcontroller.

CrossPoint Reader is a purpose-built firmware designed to be a drop-in, fully open-source replacement for the official
Xteink firmware. It aims to match or improve upon the standard EPUB reading experience.

![](./docs/images/cover.jpg)

## Motivation

E-paper devices are fantastic for reading, but most commercially available readers are closed systems with limited
customisation. The **Xteink X4** is an affordable, e-paper device, however the official firmware remains closed.
CrossPoint exists partly as a fun side-project and partly to open up the ecosystem and truly unlock the device's
potential.

CrossPoint Reader aims to:

- Provide a **fully open-source alternative** to the official firmware.
- Offer a **document reader** capable of handling EPUB content on constrained hardware.
- Support **customisable font, layout, and display** options.
- Run purely on the **Xteink X4 hardware**.

This project is **not affiliated with Xteink**; it's built as a community project.

## Features & Usage

- [x] EPUB parsing and rendering (EPUB 2 and EPUB 3)
- [x] Image support within EPUB
- [x] Saved reading position
- [x] File explorer with file picker
  - [x] Basic EPUB picker from root directory
  - [x] Support nested folders
  - [ ] EPUB picker with cover art
- [x] Custom sleep screen
  - [x] Cover sleep screen
- [x] Wifi book upload
- [x] Wifi OTA updates
- [x] KOReader Sync integration for cross-device reading progress
- [x] Configurable font, layout, and display options
  - [ ] User provided fonts
  - [ ] Full UTF support
- [x] Screen rotation

Multi-language support: Read EPUBs in various languages, including English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages).

See [the user guide](./USER_GUIDE.md) for instructions on operating CrossPoint, including the
[KOReader Sync quick setup](./USER_GUIDE.md#365-koreader-sync-quick-setup).

For more details about the scope of the project, see the [SCOPE.md](SCOPE.md) document.

## Installing

### Web (latest firmware)

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
2. Go to https://xteink.dve.al/ and click "Flash CrossPoint firmware"

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Web (specific firmware version)

1. Connect your Xteink X4 to your computer via USB-C
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Go to https://xteink.dve.al/ and flash the firmware file using the "OTA fast flash controls" section

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Manual

See [Development](#development) below.

## Development

### Prerequisites

- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- USB-C cable for flashing the ESP32-C3
- Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command.

```sh
pio run --target upload
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

after that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based
on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── stats.bin        # Per-book reading statistics (time, sessions, pages turned)
│   ├── cover.bmp        # Book cover image (once generated)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents, etc.)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, all text layout info, etc.)
│       ├── 1.bin        #     files are named by their index in the spine
│       └── ...
│
└── epub_189013891/
```

Deleting the `.crosspoint` directory will clear the entire cache.

Due the way it's currently implemented, the cache is not automatically cleared when a book is deleted and moving a book
file will use a new cache directory, resetting the reading progress.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

## Contributing

Contributions are very welcome!

If you are new to the codebase, start with the [contributing docs](./docs/contributing/README.md).

If you're looking for a way to help out, take a look at the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).
If there's something there you'd like to work on, leave a comment so that we can avoid duplicated effort.

Everyone here is a volunteer, so please be respectful and patient. For more details on our governance and community
principles, please see [GOVERNANCE.md](GOVERNANCE.md).

### To submit a contribution:

1. Fork the repo
2. Create a branch (`feature/dithering-improvement`)
3. Make changes
4. Submit a PR

---

CrossPoint Reader is **not affiliated with Xteink or any manufacturer of the X4 hardware**.

Huge shoutout to [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader), which was a project I took a lot of inspiration from as I
was making CrossPoint.
