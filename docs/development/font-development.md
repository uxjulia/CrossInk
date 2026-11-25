---
title: Font Development and Conversion
parent: Development
nav_order: 7
---

# Font Development and Conversion

For installing fonts, see [SD Card Fonts](../sd-card-fonts.md).
Run the commands below from the firmware repository root.

### Generating dictionary font families (Advanced)

Use the dictionary-specific builder to generate the complete family catalog with
the extra coverage used by dictionary definitions:

    python3 -m pip install -r lib/EpdFont/scripts/requirements.txt
    python3 lib/EpdFont/scripts/build-dictionary-fonts.py \
      --output-dir ./generated-dictionary-fonts \
      --clean \
      --jobs 2

The dictionary build includes the `reading` ranges and the built-in ranges, plus
IPA and phonetic-extension characters (`U+0250–U+02FF` and `U+1D00–U+1DBF`) and
combining-mark ranges (`U+1DC0–U+1DFF`, `U+20D0–U+20FF`, and
`U+FE20–U+FE2F`).

The default output is `../crossink-fonts/dictionary-fonts`. Use a separate
`--output-dir` for personal builds, because `--clean` removes the selected output
directory before generating the fonts. The output contains family folders and ZIP
archives; copy a family folder or unzip its archive into `/.fonts/` or `/fonts/`
on the SD card. Use `--only FamilyA,FamilyB` to generate selected families.

## Converting Custom Fonts with Python (Advanced)

This section is for people comfortable with the command line. The steps below
create `.cpfont` packs. X4 Classic, X4 Pro, and Sticky can use static `.ttf` or
`.otf` files directly instead.

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset        | Coverage                                                                                                             |
| ------------- | -------------------------------------------------------------------------------------------------------------------- |
| `ascii`       | U+0020–U+007E (Basic Latin)                                                                                          |
| `latin1`      | U+0080–U+00FF (Latin-1 Supplement)                                                                                   |
| `latin-ext`   | European languages (Latin + Extended-A/B + punctuation + currency + letterlike symbols + minus sign + ligatures)                                                  |
| `greek`       | Greek + Extended Greek                                                                                               |
| `cyrillic`    | Cyrillic + Supplement                                                                                                |
| `hebrew`      | Hebrew + Alphabetic Presentation Forms                                                                               |
| `georgian`    | Georgian + Georgian Supplement                                                                                       |
| `armenian`    | Armenian                                                                                                             |
| `ethiopic`    | Ethiopic + Extended                                                                                                  |
| `vietnamese`  | Vietnamese subset (ơ/ư and combining marks)                                                                          |
| `punctuation` | General punctuation (U+2000–U+206F)                                                                                  |
| `cjk`         | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth                                                             |
| `hangul`      | Korean Hangul syllables + Jamo + Compatibility Jamo                                                                  |
| `cherokee`    | Cherokee (historic + supplement block)                                                                               |
| `tifinagh`    | Tifinagh                                                                                                             |
| `symbols`     | Math, currency, arrows, box-drawing, misc symbols, dingbats                                                          |
| `reading`     | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin`     | Matches the firmware's built-in font conversion intervals                                                            |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint`: force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.


## Scalable-font implementation and attribution

The `.otf` discovery and web upload support follows
[CrossPoint runtime SD fonts #3646](https://github.com/crosspoint-reader/crosspoint-reader/pull/3646)
and [web font upload #3717](https://github.com/crosspoint-reader/crosspoint-reader/pull/3717).
Upstream accepts the `.otf` extension but its FreeInk SDK has no CFF (OpenType
outline) driver, so extension support alone is not full OpenType support.

CrossInk's S3 builds enable the SDK's CFF driver (`FREEINK_FONT_ENABLE_CFF`), so
static OpenType CFF fonts really render, including CFF fonts named `.ttf`. The
existing PSRAM feature gates, style metadata, font memory arenas, and
content-based cache identities are kept. The font catalog version changed so old
discovery results are rebuilt. The **TTF Rendering** menu also controls
OpenType faces.

## Fonts embedded in EPUBs

On a PSRAM-equipped reader (such as X4 Pro or Sticky), open a book, then open
the reader menu → Layout → Publisher fonts:

- **Off**: Use your selected reading font instead of the book's embedded fonts.
- **Ignore Body Font** (default): Use your selected reading font instead of the
  publisher's main body font. Allow other embedded fonts from the book. Text
  using the same family as the body also uses your selected reading font.
- **All Text**: Use the book's embedded fonts wherever specified and supported,
  with your selected reading font as the fallback.

Reader settings also provide the global default; a book's choice is saved
separately. Embedded Style must be enabled, and Light/Safe Mode keeps the
reader font.

The EPUB must contain static TTF or OTF font files referenced by CSS
`@font-face` rules. Merely naming an installed font in `font-family` does not
embed it. WOFF, WOFF2, variable fonts and obfuscated/encrypted font data are
not supported. Ignore Body Font identifies the body family from the chapter's
root style or a bounded scan of its opening paragraphs; unusual typography
may require All Text or Off. Compact tables continue using the reader font.

The loader keeps metadata for at most 32 faces and loads a family only when
layout uses it. Ignored body families consume no font-file budget. Loaded
faces are limited to 2 MiB each and 4 MiB per book, subject to the existing
scalable-font memory reserve. Unsupported
or unavailable faces fall back to the selected reader font. X3/X4 do not
load embedded fonts. Extracted files live in the book cache and can be
removed with that book's cache. Layout caches rebuild when the publisher
mode or required font data changes. Cached sections remember the families
requested during their reader session, restore them before reuse, and rebuild
when font availability changes.

To run the host regression fixture, generate it with
`python3 scripts/generate_publisher_font_epub.py /tmp/publisher-fonts.epub`, then run
`CROSSINK_SIMULATOR_SMOKE_PUBLISHER_FONTS=1 python3 scripts/run_simulator_smoke_test.py --env x4-pro-simulator --no-build --book /tmp/publisher-fonts.epub --status-bar-captures /tmp/publisher-font-captures`.
Build `x4-pro-simulator` first. This checks modes, portrait/landscape layout,
page-cache round trips, font identity invalidation, saved settings migration,
menu choices and reader-session cleanup;
it does not establish physical-device memory or SD-card performance.
