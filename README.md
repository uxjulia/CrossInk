> **This is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** with a focus on improved fonts and minimal reading stats.

## What's different in this fork

My goal with this fork was to maintain the core Crosspoint firmware while integrating my preferred typography and some lightweight reading statistics. I’ve focused on keeping the underlying system stable while layering in a few "nice-to-have" features and UI refinements along the way.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: Small, Margin: 15" /><br/>
      <em>Font: Bitter, Size: Small, Margin: 15</em>
    </td>
    <td align="center">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats with custom front button mapping shown" /><br/>
      <em>Reading Stats with custom front button mapping shown</em>
    </td>
  </tr>
</table>

### Summary

- New reader fonts: ChareInk, Lexend Deca, and Bitter
- Unicode emoji and miscellaneous symbols support
- Adjusted font sizes: Tiny (10pt), Small (12pt), Medium (14pt), Large (16pt), Extra Large (18pt). See [Font Sizes](#font-sizes) for more details.
- Added ~~strikethrough~~ support
- Made <u>underlines</u> thicker for better visibility
- Added ability to add bookmarks
- Added ability to remap front buttons that only applies in the reader
- Added Bionic Reading and Guide Dots as optional reader modes
- Added Force Paragraph Indents for books that render as one giant wall of text
- Added more in-reader control remapping options for side buttons, short power button clicks, and long-press menu actions
- Added ability to mark a book as finished from the in-book menu. A pop-up will also display once 99% of the book is reached. This status allows tracking of total books read.
- Added ability to move finished books to "Read" folder
  - To turn this on, go to Settings > System > Move finished books to Read folder. Once a book is marked as finished, the book will be moved to the folder when the book is closed.
- In-book menu to quickly adjust reader options without having to exit the book
- Reading stats: total books read, total reading time, number of sessions, pages turned, average session time, pages turned per minute
- Changed label for "Auto Turn (Pages Per Minute)" to "Auto Page Turn Interval (seconds)"
  - Added additional page turn intervals (how many seconds pass between page turns). Options are now (in seconds): 60, 45, 30, 20, 15, 10, 5, OFF.
- Added Vietnamese language support
- Device simulator during development

---

### Reader Fonts

The default fonts have been replaced with ChareInk, Lexend Deca, and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [ChareInk](https://www.mobileread.com/forums/showthread.php?t=184056) - A cult favorite among the e-reading community for over a decade based off of the typeface [Charis](https://software.sil.org/charis/). It is specially designed to make long texts pleasant and easy to read.
- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4.

The UI now uses [Inter](https://fonts.google.com/specimen/Inter) as the display font which has improved readability at smaller sizes.

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

### Bionic Reading

This feature will bold the initial letters or parts of words, creating "artificial fixation points" that can make it easier to let your brain fill in the rest of the word without having to focus on every letter. You can toggle it from **Reader settings**.

This was merged from [CrossPoint PR 1670](https://github.com/crosspoint-reader/crosspoint-reader/pull/1670).

### Guide Dots

This feature adds small dots between every word. The idea comes from the book [Speed Reading: Learn to Read a 200+ Page Book in 1 Hour](https://amzn.to/4mOPSJo): by focusing on the space between words instead of the words themselves, your peripheral vision can pick up more of the text. You can toggle it from **Reader settings**.

### Force Paragraph Indents

Have you ever opened a book and the paragraph indents just were not rendering, leaving you with an overwhelming wall of text? That usually happens because some publishers do not define their indents in ways the firmware understands. This setting forces each new paragraph to have an indent regardless of how the book is formatted.

This works when **Reader Paragraph Alignment** is set to **Left**, **Justify**, or **Book's Style**. You can toggle it from **Reader settings**.

### New settings in Controls

#### Side Buttons Long Press

Previously, **Long-press Chapter Skip** applied to both the front and side buttons. I split this out so the side buttons can do their own thing, which opens up a lot more flexibility.

You can now choose what the side buttons do when you long-press them:

- **Change Font Size**: Up increases the font size, Down decreases it
- **Chapter Skip**: Previous behavior
- **Ignore**: Do nothing

#### Short Power Button Click

There is now a new short power button action:

- **Change Font**: Cycles through the fonts one by one

#### Long Press Menu Action

While you are in the reader, you can now set the Confirm/Menu button to do one of these actions when long-pressed:

- **Change Font**: Cycles through the fonts one by one
- **Guide Dots**: Turns Guide Dots on or off
- **Bionic Reading**: Turns Bionic Reading on or off
- **Toggle Bookmark**: Adds or removes a bookmark from the current page
- **Refresh Screen**: Forces a screen refresh
- **Sync Progress**: Syncs KOReader progress
- **Mark as Finished**: Marks the current book as finished/unfinished
- **Reading Stats**: Opens the reading stats screen
- **Take Screenshot**: Takes a screenshot
- **Cycle Page Turn**: Cycles through the auto page turn intervals: **Off → 5s → 10s → 15s → 20s → 30s → 45s → 60s → Off**

### Reading stats

Some simple per-book reading stats are tracked automatically and displayed in two places:

**In-book menu → Reading Stats:**

- Total reading time
- Number of sessions
- Pages turned
- Average session time
- All time reading stats including total number of books read

**Home screen book card (Lyra theme only):**

- Total reading time
- Average session time

### Finished books / Read folder

- You can manually mark a book as finished from the in-book menu
- At 99% book progress a pop-up will also display asking if you want to mark the book as finished
- If you have the "Move finished books to Read folder" setting turned on, then once you have marked a book as finished, the book will automatically be moved to a folder named "Read" on your SD card
- Marking books as finished also enables the total "Books Read" reading stat

### Language Support

- Added language support for Vietnamese. This addresses [issue #34](https://github.com/uxjulia/CrossInk/issues/34).

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
5. Choose the `firmware-*.bin` file you downloaded and click "Flash"

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
