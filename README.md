> **This is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** with a focus on improved fonts and minimal reading stats.

## What's different in this fork

My goal with this fork was to maintain the core Crosspoint firmware while integrating my preferred typography and some lightweight reading statistics. I’ve focused on keeping the underlying system stable while layering in a few "nice-to-have" features and UI refinements along the way.

### Summary

- New reader fonts: ChareInk, Lexend Deca, and Bitter
- Unicode emoji and miscellaneous symbols support
- Adjusted font sizes: Tiny (10pt), Small (12pt), Medium (14pt), Large (16pt), Extra Large (18pt). See [Font Sizes](#font-sizes) for more details.
- Added ~~strikethrough~~ support
- Made <u>underlines</u> thicker for better visibility
- Added ability to add bookmarks
- Added ability to remap front buttons that only applies in the reader
- In-book menu to quickly adjust font options without having to exit the book
- Reading stats: total reading time, number of sessions, pages turned, average session time, pages turned per minute
- Changed label for "Auto Turn (Pages Per Minute)" to "Auto Page Turn Interval (seconds)"
  - Added additional page turn intervals (how many seconds pass between page turns). Options are now (in seconds): 60, 45, 30, 20, 15, 10, 5, OFF.
- Device simulator during development

---

### Reader Fonts

The default fonts have been replaced with ChareInk, Lexend Deca, and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [ChareInk](https://www.mobileread.com/forums/showthread.php?t=184056) - A cult favorite among the e-reading community for over a decade based off of the typeface [Charis](https://software.sil.org/charis/). It is specially designed to make long texts pleasant and easy to read.
- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4.

The UI now uses [DM Sans](https://fonts.google.com/specimen/DM+Sans) as the display font and the smallest text on the UI has been replaced with [Inter](https://fonts.google.com/specimen/Inter), both fonts have improved readability at smaller sizes.

### Emojis and Misc Glyphs

- Support for Unicode [Emoticons](https://unicode-explorer.com/b/1F600) and [Miscellaneous Symbols](https://unicode-explorer.com/b/2600) using [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji) and [Noto Sans Symbols](https://fonts.google.com/noto/specimen/Noto+Sans+Symbols) font.

---

### Font Sizes

There are 3 available build variants to choose from due to build size constraints: tiny, xlarge, and no_emoji

**tiny**

> No Extra Large font size. My preferred build.

- Emoji & Misc. Symbols Support
- 4 Font sizes:
  - Tiny (10pt)
  - Small (12pt)
  - Medium (14pt)
  - Large (16pt)

**xlarge**

> Tiny and Small font sizes had to be removed to reduce build size and still support emoji/symbols.

- Emoji & Misc. Symbols Support
- 3 Font sizes:
  - Medium (14pt)
  - Large (16pt)
  - Extra Large (18pt)

**no_emoji**

> All font sizes available, but no emoji/symbols support.

- **No** Emoji & Misc. Symbols Support
- 5 Font sizes:
  - Tiny (10pt)
  - Small (12pt)
  - Medium (14pt)
  - Large (16pt)
  - Extra Large (18pt)

---

### Reader options in the in-book menu

Reader settings (font, size, line spacing, margins, alignment, etc.) are now accessible directly from the in-book menu without leaving the book. Open the menu while reading and select **Reader Options** to adjust any reader setting on the spot. Changes take effect immediately.

### Reading stats

Some simple per-book reading stats are tracked automatically and displayed in two places:

**In-book menu → Reading Stats:**

- Total reading time
- Number of sessions
- Pages turned
- Average session time
- All time reading stats

**Home screen book card (Lyra theme only):**

- Total reading time
- Average session time

---

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

## Installing

### Web

1. Download the `firmware-*.bin` file for the build variant of your choosing from the [releases](https://github.com/uxjulia/CrossInk/releases) page
2. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
3. Go to https://crosspointreader.com/#flash-tools and choose your device
4. Select "Custom .bin" from the options
5. Choose the `firmware-*.bin` file you uploaded and click "Flash"

To revert back to the official firmware, you can flash the latest official firmware from https://crosspointreader.com/#flash-tools

### Command line (specific firmware version)

> **Note:** These instructions are for macOS and Linux. Windows users should use the [Web installer](#web) instead.

1. Install [`esptool`](https://github.com/espressif/esptool) :

```bash
pip3 install esptool
```

2. Download the `firmware-*.bin` file from the release of your choice via the [releases](https://github.com/uxjulia/CrossInk/releases)
3. Connect your Xteink X4 to your computer via USB-C.
4. Note the device location. On Linux, run `dmesg | grep tty` after connecting. On macOS, run `ls /dev/cu.*` before and after connecting — the new entry is your device (typically `/dev/cu.usbmodem*`).

5. Flash the firmware :

```bash
# Update the device port with your actual device port (/dev/...) from step 4

# Linux
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin

# macOS
esptool.py --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

## Development

### Prerequisites

- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- USB-C cable for flashing the ESP32-C3
- Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/uxjulia/CrossInk

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command. Replace `tiny` with `xlarge` or `no_emoji` if you prefer a different build variant (see [Font Sizes](#font-sizes)).

```sh
pio run -e tiny --target upload
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

# For macOS (replace with your device path from ls /dev/cu.*)
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

## Internals

The firmware is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
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
