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

---

**Note**: This firmware is confirmed to be working on both the X3 and X4.

### Highlights

- New reader fonts: ChareInk, Lexend Deca, and Bitter.
- Unicode emoji and miscellaneous symbols support (a limited subset).
- Adjusted font sizes: Teensy (8pt), Itty Bitty (9pt), Tiny (10pt), Small (12pt), Medium (14pt), Large (16pt), Extra Large (18pt), Huge (20pt). See [Font Sizes](#font-sizes) for more details.
- Added ~~strikethrough~~ support.
- Made <u>underlines</u> thicker for better visibility.
- Added a custom `Minimal` theme and sleep screen option for the minimalists out there.
- Added support for `<hr>` section breaks.
- Added support for "redaction" style rendering.
- Added improved support for tables with simple markup.
- Added ability to add bookmarks.
- Added ability to remap front buttons that only applies in the reader.
- Added Bionic Reading and Guide Dots as optional reader modes.
- Added Force Paragraph Indents for books that render as one giant wall of text.
- Added ability to pin a sleep image as a favorite. The favorited image will always be displayed when your sleep settings are set to `Custom` or `Cover + Custom` (when no cover is available).
- Added more in-reader control remapping options for side buttons, short power button clicks, and long-press menu actions.
- Added ability to mark a book as finished from the in-book menu. A pop-up will also display once 99% of the book is reached. This status allows tracking of total books read.
- Added ability to move finished books to "Read" folder.
- In-book menu to quickly adjust reader options without having to exit the book.
- Reading stats: total books read, total reading time, number of sessions, pages turned, average session time, pages turned per minute. You can also set your reading stats as your sleep screen.
- Reading stats [syncing](./docs/reading-stats-sync.md) between two devices.
- Added customizable Auto Page Turn Interval (anything between 5-120 seconds).
- Added ability to view Recent Books as a 3x3 grid view.
- To view a more detailed list for each version, visit the [releases](https://github.com/uxjulia/CrossInk/releases) page to read release notes.

---

### Reader Fonts

The default fonts have been replaced with ChareInk, Lexend Deca, and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4/X3 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [ChareInk](https://www.mobileread.com/forums/showthread.php?t=184056) - A cult favorite among the e-reading community for over a decade based off of the typeface [Charis](https://software.sil.org/charis/). It is specially designed to make long texts pleasant and easy to read.
- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4/X3.

The UI now uses [Inter](https://fonts.google.com/specimen/Inter) as the display font which has improved readability at smaller sizes.

### Emojis and Misc Glyphs

- Support for a limited set of Unicode [Emoticons](https://unicode-explorer.com/b/1F600) and [Miscellaneous Symbols](https://unicode-explorer.com/b/2600) using [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji) and [Noto Sans Symbols](https://fonts.google.com/noto/specimen/Noto+Sans+Symbols) font.

---

### Font Sizes

There are 4 available build variants to choose from due to build size constraints: tiny, xlarge, and no_emoji

**teensy**
> Only the small sized fonts.
- Emoji & Misc. Symbols Support
- 4 Font sizes:
  - Teensy (8pt)
  - Itty Bitty (9pt)
  - Tiny (10pt)
  - Small (12pt)

**tiny**

> No Extra Large or Huge font size. My preferred build.

- Emoji & Misc. Symbols Support
- 4 Font sizes:
  - Tiny (10pt)
  - Small (12pt)
  - Medium (14pt)
  - Large (16pt)

**xlarge**

> Teensy, Tiny, and Small font sizes had to be removed to reduce build size and still support emoji/symbols.

- Emoji & Misc. Symbols Support
- 3 Font sizes:
  - Large (16pt)
  - Extra Large (18pt)
  - Huge (20pt)

**no_emoji**

> All standard font sizes from Tiny through Extra Large are available, but no emoji/symbols support.

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

### Guide Dots

This feature adds small dots between every word. The idea comes from the book [Speed Reading: Learn to Read a 200+ Page Book in 1 Hour](https://amzn.to/4mOPSJo): by focusing on the space between words instead of the words themselves, your peripheral vision can pick up more of the text. You can toggle it from **Reader settings**.

### Force Paragraph Indents

Have you ever opened a book and the paragraph indents just were not rendering, leaving you with an overwhelming wall of text? That usually happens because some publishers do not define their indents in ways the firmware understands. This setting forces each new paragraph to have an indent regardless of how the book is formatted.

This works when **Reader Paragraph Alignment** is set to **Left**, **Justify**, or **Book's Style**. You can toggle it from **Reader settings**.

## Custom button actions

The Controls menu in Settings has been updated to the following

<u>**Power Button**</u>
- Short-press Action - **New Options Added**
- Long-press Action - **New**

<u>**Front Buttons**</u>
- Remap Front Buttons
- Remap Front Buttons (reader)
- Long-press Menu Action - **New**

<u>**Side Buttons**</u>
- Layout
- Long-press Chapter Skip
- Long-Press Action - **New**

---

**Side Button Long Press Action** - Use the side buttons to change your font size. Previously, the "Long-press Chapter Skip" applied to both the front and side buttons. I've split this out so now you can change your font size when you long-press them. Press and hold for about 2 seconds: Up to increase font size, Down to decrease font size. Default = Chapter Skip

**Short-press Power Button Action** - Default = Ignore
**Long-press Power Button Action** - Default = Sleep
**Long Press Menu Button Action** (This is the Menu/Confirm button when you are in the reader): Default = Ignore

Map the **Power** or **Menu** button short/long-press action to one of the following options:
- Ignore
- Sleep
- Page Turn
- Refresh Screen
- Change Font (cycles through the fonts one by one)
- Guide Dots (turns guide dots on/off)
- Bionic Reading (turns bionic reading on/off)
- Toggle Bookmark (adds or removes a bookmark from the current page)
- Sync Progress (syncs KoReader progress)
- Mark as Finished (marks book as finished)
- Reading Stats (displays reading stats)
- Take Screenshot (takes a screenshot)
- Auto Page Turn Interval (opens the selection for auto page turn interval)
- File Transfer (opens the File Transfer menu)
- Tilt Page Turn (turns tilt-based page turning on/off on supported devices)

### Reading stats

Some simple per-book reading stats are tracked automatically and displayed in two places:

**In-book menu → Reading Stats:**

- Total reading time
- Number of sessions
- Pages turned
- Average session time
- All time reading stats including total number of books read

### Finished books / Read folder

- You can manually mark a book as finished from the in-book menu
- At 99% book progress a pop-up will also display asking if you want to mark the book as finished
- If you have the "Move finished books to Read folder" setting turned on, then once you have marked a book as finished, the book will automatically be moved to a folder named "Read" on your SD card
- Marking books as finished also enables the total "Books Read" reading stat

### Language Support

- Added language support for Vietnamese.

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
## Installation
### Web

1. Download the `firmware-*.bin` file for the build variant of your choosing from the [releases](https://github.com/uxjulia/CrossInk/releases) page
2. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
3. Go to https://crosspointreader.com/#flash-tools and choose your device
4. Select "Custom .bin" from the options
5. Choose the `firmware-*.bin` file you downloaded and click "Flash"

To revert back to the official firmware, you can flash the latest official firmware from https://crosspointreader.com/#flash-tools

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

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
### Revert to Official Firmware

To revert to the official firmware, you can flash the latest official firmware using https://crosspointreader.com/#flash-tools.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Common issues](./docs/troubleshooting.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

### Prerequisites

- **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
- Python 3.8+
- USB-C cable for flashing the ESP32-C3
- Unlocked Xteink X4 or X3

### Setup

CrossInk uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/uxjulia/CrossInk

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / flash / monitor

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

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS (replace with your device path from ls /dev/cu.*)
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

The firmware is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based
on this constraint.

### Data caching

The first time a book is loaded, CrossInk writes reusable data to the SD card so later opens do not have to rebuild
everything from scratch. This data lives in `.crosspoint` on the SD card. The directory also contains device settings,
saved servers, recent books, bookmarks, and reading stats, so it is more than just a disposable render cache.

The structure is roughly:

```
.crosspoint/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── synced_stats/           # Stats snapshots received from other readers
├── settings.bin            # Device settings
├── state.bin               # Last-opened book and sleep/session state
├── recent.bin              # Recent books list
├── bookmarks/              # Bookmark files, one per book
├── home_carousel_cache.bin # Lyra Carousel home-screen snapshot cache
├── epub_12471232/          # Each EPUB is cached to `epub_<hash>`
│   ├── progress.bin        # Reading position (chapter, page, etc.)
│   ├── stats.bin           # Per-book reading stats
│   ├── cover.bmp           # Book cover image, once generated
│   ├── thumb_*.bmp         # Home/recent-books thumbnail images
│   ├── book.bin            # Book metadata (title, author, spine, table of contents, etc.)
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

Deleting the entire `.crosspoint` directory will reset caches, settings, saved network/server data, bookmarks, recent
books, reading progress, and reading stats. To clear EPUB/XTC render caches from the device UI, use
**Settings > System > Clear Reading Cache**; that leaves settings and global stats in place.

Due to the way it's currently implemented, cache data is not automatically cleared when a book is deleted. Moving a book
file creates a new path-based cache directory, so the moved copy may start with fresh reading progress.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).
