---
title: SD Card Fonts
nav_order: 4
---

# SD Card Fonts

CrossInk can load extra fonts from the SD card, including fonts that cover other
writing systems (Chinese/Japanese/Korean, Cyrillic, Greek, and more).

- **Every reader** can use `.cpfont` font packs. These are fonts converted for
  CrossInk, with one file per size.
- **X4 Classic, X4 Pro, and Sticky** can also use ordinary static TrueType
  (`.ttf`) and OpenType (`.otf`) font files directly, in any size. The original
  X3 and X4 need `.cpfont` files.

## Available Pre-Built Fonts

You can view pre-built fonts available for download at [Inky](https://inky.crossink.dev/#downloads).

## Converting Custom Fonts with CrossPoint's Font Builder

To make `.cpfont` packs from your own TrueType/OpenType fonts, use CrossPoint's
[Font Builder](https://crosspointreader.com/fonts). On X4 Classic, X4 Pro, and
Sticky, static `.ttf` and `.otf` files do not need conversion.

The local `fontconvert_sdcard.py --intervals latin-ext` preset includes combining
accent marks (U+0300–U+036F). Regenerate older latin-ext packs from their original
source fonts to add these glyphs; the source font must contain them. Preserve the
source font license when distributing regenerated packs. Built-in and `reading`
presets already include this range and do not need regeneration for this fix.
The maintained `build-sd-fonts.py` pipeline adds `builtin`, so its existing packs
also already request these marks.

## Installing Fonts

There are three ways to install fonts:

### Option 1: Manual SD card copy (Fastest)

1.  Download pre-built `.cpfont` files from [Inky](https://inky.crossink.dev/#downloads),
    or use your own static `.ttf` or `.otf` files on an X4 Classic, X4 Pro, or Sticky.
    - If you downloaded a `.zip`, extract it first.
2.  Copy font family folders to one of two locations on your SD card:
    - `/.fonts/` - hidden folder (preferred; keeps the SD card tidy when
      hidden files are not shown on your device)
    - `/fonts/` - visible folder (use this if your computer hides folders that
      start with a dot and you would rather see it in your file manager)

    Both folders are checked when the reader starts and the results are
    combined: a family installed in `/fonts/` shows up even when `/.fonts/` also
    exists, and the other way round. If the same family name appears in both,
    the copy in `/.fonts/` wins and the duplicate in `/fonts/` is ignored. Put
    each font family in its own folder, including `.ttf` and `.otf` families.
    Do not mix `.cpfont` with `.ttf` or `.otf` files in one family folder.

        SD Card Root/
        ├── .fonts/                     <- Hidden folder (preferred)
        │   └── Literata/
        │       ├── Literata_12.cpfont
        │       ├── Literata_14.cpfont
        │       ├── Literata_16.cpfont
        │       └── Literata_18.cpfont
        └── fonts/                      <- Visible folder (equally valid)
            └── Merriweather/
                ├── Merriweather_12.cpfont
                └── ...

    On an X4 Classic, X4 Pro, or Sticky, a `.ttf` or `.otf` family can look like
    `/fonts/MyFont/MyFont-Regular.ttf` alongside its bold, italic, and bold
    italic files.

3.  Insert the SD card and power on your CrossInk device. If it was already on,
    restart it so the new fonts are found.

### Option 2: Download from device

This option downloads pre-built `.cpfont` packs straight to the reader.

1. Go to **Settings > Reader > Font Options > Download Fonts**
2. Connect to Wi-Fi if asked
3. Browse the available font families and select one to download
4. Downloaded fonts appear in **Settings > Reader > Font Options > Font Family**

> [!NOTE]
> To change which font sizes are downloaded, change **Download Font Size Range**
> (in the same **Font Options** menu) _before_ downloading.

### Option 3: Upload via web browser

1. Start **File Transfer** and connect through **Join a Network** or **Create Hotspot**
2. Open the web address shown on the reader (see the [Web Server Guide](./webserver.md))
3. Open the **Fonts** tab
4. Select a folder containing one font family, then upload its `.cpfont` files
   or, on an X4 Classic, X4 Pro, or Sticky, its static `.ttf` or `.otf` files. The
   page only offers `.ttf` and `.otf` uploads on readers that support them.

For the full range of text styles with a `.ttf` or `.otf` family, include regular,
bold, italic, and bold italic files. Variable fonts are not supported. Each file
must be 2 MiB or smaller, and the family must total 6 MiB or less. See
[Scalable Fonts](./scalable-fonts.md) for more about using them.

## Dictionary Fonts

EPUB books can use a different installed SD-card family for dictionary definitions.
Set it for all books in **Settings > Reader > Font Options > Dictionary Font**, or for
one book from the reader menu's font options. If a saved point size is no longer
available, CrossInk chooses the closest size from the dictionary family. When the
reader is low on memory, the dictionary may show in your reader font instead. This
is normal. See [Dictionary](./dictionary.md) for more.

Choose a font that includes pronunciation symbols if you use a dictionary.
Prepared dictionary fonts are available from [Inky](https://inky.crossink.dev/#downloads).

### Damaged font files

If the EPUB reader shows **Damaged font file**, a `.cpfont` file is damaged. Some characters may appear blank. Delete the font family folder and install it again. Clear the affected book’s reading cache if its spacing was built using the damaged font.
