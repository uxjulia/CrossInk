---
title: SD Card Fonts
nav_order: 12
---

# SD Card Fonts

CrossInk supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your device to WiFi
2. Go to **Settings > System > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

Before downloading, choose **Settings > Reader > Downloaded Font Size Range**.
That setting controls which `.cpfont` point sizes are downloaded for each
family:

- teensy: 8, 9, 10, 12 pt
- tiny: 10, 12, 14, 16 pt
- xlarge: 16, 18, 20 pt
- no_emoji: 10, 12, 14, 16, 18 pt
- all: 8, 9, 10, 12, 14, 16, 18, 20 pt

Only the files in the selected range are downloaded. For example, if the range
is `xlarge`, downloading Lexend Deca installs only:

    Lexend Deca_16.cpfont
    Lexend Deca_18.cpfont
    Lexend Deca_20.cpfont

Downloaded families include the range in their installed family name, so you
can keep multiple ranges for the same family. For example, downloading Lexend
Deca with the `teensy` range installs **Lexend Deca (teensy)**, and downloading
it again with `xlarge` installs **Lexend Deca (xlarge)**.

Fonts installed before range names were added still work. If you already have
an older folder such as `/.fonts/Lexend Deca/`, CrossInk will keep showing it
as an installed download when it contains the files for the currently selected
range. New downloads use the range-suffixed folder names.

On the SD card, downloaded fonts are stored under `/.fonts/` by default:

    SD Card Root/
    └── .fonts/
        ├── Lexend Deca (teensy)/
        │   ├── Lexend Deca_8.cpfont
        │   ├── Lexend Deca_9.cpfont
        │   ├── Lexend Deca_10.cpfont
        │   └── Lexend Deca_12.cpfont
        └── Lexend Deca (xlarge)/
            ├── Lexend Deca_16.cpfont
            ├── Lexend Deca_18.cpfont
            └── Lexend Deca_20.cpfont

Each range-scoped folder appears as a separate option in **Settings > Reader >
Font Family**. When one of those families is selected, **Font Size** changes to
match the sizes installed in that folder.

### Option 2: Upload via web browser

1. Open **File Transfer** on your device and choose **Join Network** or **Create Hotspot**
2. Open the web interface in your browser (shown on the WiFi screen)
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crossink-fonts repository](https://github.com/uxjulia/crossink-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata (tiny)/
       │       ├── Literata_10.cpfont
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       └── Literata_16.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your device

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crossink-fonts repository](https://github.com/uxjulia/crossink-fonts).

## Converting Custom Fonts
1. Using the official Crosspoint Font tool: https://crosspointreader.com/fonts

2. Convering TrueType/OpenType fonts on your computer:

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

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020-U+007E (Basic Latin) |
| `latin-ext` | European languages (Latin + Extended-A/B) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables |
| `builtin` | Matches built-in Bookerly coverage exactly |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

Install custom fonts via WiFi upload or manual SD card copy.

When a custom font family contains multiple `.cpfont` sizes, the reader maps
font-size steps onto those files from smallest to largest.
