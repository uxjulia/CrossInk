> **This is a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** with a focus on improved fonts and minimal reading stats.

## What's different in this fork

My goal with this fork was to maintain the core Crosspoint firmware while integrating my preferred typography and some lightweight reading statistics. I’ve focused on keeping the underlying system stable while layering in a few "nice-to-have" features and UI refinements along the way.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: 12 pt, Margin: 15" /><br/>
      <em>Font: Bitter, Size: 12 pt, Margin: 15</em>
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
- Adjusted font sizes: 8 pt, 9 pt, 10 pt, 12 pt, 14 pt, 16 pt, 18 pt, and 20 pt. See [Font Build Variants](./docs/font-build-variants.md) for more details.
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

There are 4 available build variants to choose from due to build size constraints: `teensy`, `tiny`, `xlarge`, and `no_emoji`.

See [Font Build Variants](./docs/font-build-variants.md) for the full point-size and emoji-support matrix.

---

### Reader features

Reader Options, Bionic Reading, Guide Dots, Force Paragraph Indents, reading stats, and finished-book behavior are documented in [Reader Features](./docs/reader-features.md).

## Custom button actions

CrossInk adds configurable button shortcuts.

See [Controls](./docs/controls.md) for the full action list and defaults.


### Development Device Simulator

The [device simulator](https://github.com/uxjulia/crosspoint-simulator) renders the e-ink display in an SDL2 window so firmware changes can be sanity-checked without flashing hardware.

See [Simulator](./docs/simulator.md) for setup, platform notes, keyboard controls, and cache tips.

---
## Installation

Download a `firmware-*.bin` from the [releases page](https://github.com/uxjulia/CrossInk/releases), then flash it with the web installer or command line.

See [Installation](./docs/installation.md) for step-by-step flashing and revert instructions.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Installation](./docs/installation.md)
- [Font Build Variants](./docs/font-build-variants.md)
- [Reader Features](./docs/reader-features.md)
- [Controls](./docs/controls.md)
- [Simulator](./docs/simulator.md)
- [Data Cache](./docs/data-cache.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Common issues](./docs/troubleshooting.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

CrossInk uses PlatformIO for building and flashing firmware.

See [Getting Started](./docs/contributing/getting-started.md) for prerequisites, clone setup, hooks, and validation commands.

### Build / flash / monitor

Connect your Xteink X4 or X3 via USB-C and run:

```sh
pio run -e tiny --target upload
```

Replace `tiny` with another build variant if needed. See [Font Build Variants](./docs/font-build-variants.md).

See [Testing and Debugging](./docs/contributing/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

---

## Internals

The ESP32-C3 has about 380 KB of usable RAM, so CrossInk stores reusable book and device data on the SD card instead of rebuilding everything in memory.

See [Data Cache](./docs/data-cache.md) for the `.crosspoint` layout and [File Formats](./docs/file-formats.md) for binary cache details.
