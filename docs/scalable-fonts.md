# Scalable fonts on X4 Classic, X4 Pro, and Sticky

On the X4 Classic, X4 Pro, and Sticky, CrossInk's built-in fonts (Bitter and
Lexend Deca) are scalable, so you can pick any size from 8 to 22 pt. You can also
add your own static TrueType (`.ttf`) and OpenType (`.otf`) fonts. These
readers have extra memory that makes this possible. The original X3 and X4 use
fixed-size fonts and `.cpfont` font packs instead.

## Add your own scalable fonts

1. Copy your `.ttf` or `.otf` files into a family folder on the SD card, such as
   `/fonts/My Font/`. The hidden `/.fonts/My Font/` location works too.
2. If you copied the files using a computer, put the SD card back and restart the
   reader so it finds them.
3. Choose the family from **Settings > Reader > Font Options > Font Family**, or
   from the font picker while reading.

You can also upload `.ttf` or `.otf` files through the **Fonts** tab in the reader's web
interface. See [SD Card Fonts](./sd-card-fonts.md) for how to open that interface.
No font conversion or separate files for each size are needed.

> [!NOTE]
> A "static" font file holds one fixed style, such as Regular or Bold. These are
> supported. "Variable" fonts (one file that holds many weights) and font
> collections are not.

For the full range of text styles, add all four files for a family: **regular**,
**bold**, **italic**, and **bold italic**. CrossInk can use a family with fewer
styles, but missing styles may appear in another available style. If the files
are in a family folder, that folder's name appears in the font picker. Keep
scalable fonts and `.cpfont` files in separate family folders.

## Adjust the appearance

After selecting a custom `.ttf` or `.otf` family, open **TTF Rendering** under
**Settings > Reader > Font Options**, or from the in-book font menu. These
controls (such as **Hinting**, **Weight**, **Slant**, and **Stem Darkening**)
adjust how the letters look on the e-ink screen. Changes to a family apply to
all its styles and are remembered for that family. **Reset TTF Defaults** undoes
them. This option appears only for custom `.ttf` and `.otf` fonts; `.cpfont`
fonts do not use it.

Custom fonts use the same **8-22 pt** size choices as the built-in fonts. Changing fonts or
their appearance may make CrossInk lay out the current book again, so page
positions can change. Your bookmarks and clippings remain saved.

## Supported files and limits

- Use static TrueType `.ttf` or OpenType `.otf` files. Variable fonts and font
  collections are not supported.
- Each file must be **2 MiB or smaller**, and the four files in one family must
  total **6 MiB or less**.
- If both `/fonts` and `/.fonts` contain the same family, the copy in
  `/.fonts` takes precedence.

If a font does not appear, check its file type and size, then restart the
reader. If the reader has too little free memory to open a
font, it may use a fallback font instead.

## Fonts embedded in EPUBs

On a supported reader (X4 Classic, X4 Pro, or Sticky), open a book, then open
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

The book must include supported fonts. A publisher font may be unavailable or
too large for the reader; in that case, your selected reading font is used.
If a book's typography looks wrong, try **All Text** or **Off**. Compact tables
continue to use your reading font. X3 and X4 do not use publisher fonts.

Changing this option can make the reader prepare the book again. Your saved
reading position, bookmarks, and clippings are kept.
