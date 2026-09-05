# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## `/.crosspoint/sleep-image-index/<directory-hash>-{bmp,all}.idx`

### Version 1

`ImageFolderIndex` (`src/activities/boot_sleep/ImageFolderIndex.{h,cpp}`) keeps
a compact, rebuildable index per indexed folder. It backs both the sleep-image
folder and the boot-screen folder (`/bootscreen` or `/.bootscreen`); the
directory name predates the boot-screen use and was kept as-is so existing
sleep caches aren't orphaned by an unrelated rename. The index avoids walking
a folder on every selection while using only one fixed-size record at a time
in RAM. `bmp` contains BMP files and `all` contains BMP and PNG files (sleep's
Page Overlay mode only; the boot screen is BMP-only and always uses a `bmp`
index). The `validated` header flag means BMP headers were checked while
rebuilding after a failed render.

The index is disposable: a missing, malformed, or stale selected entry causes
one rebuild and then the caller falls back to its directory scan. File
transfer, file-browser, and preferred-folder changes invalidate affected
indexes via `ImageFolderIndex::invalidateForPath()`. Files added or changed
directly on the SD card have no notification path; they are picked up when a
cached entry is found missing or when an index is otherwise rebuilt.

```c++
struct ImageFolderIndexHeader {
    char magic[4];       // "CSIX"
    u8 version;          // 1
    u8 flags;            // bit 0: BMP+PNG, bit 1: BMP headers validated
    u16 pathLength;
    u16 recordCount;
    u16 recordSize;      // sizeof(ImageFolderIndexRecord)
    u32 recordsOffset;   // sizeof(header) + pathLength
    char directory[pathLength];
};

struct ImageFolderIndexRecord {
    u16 nameLength;
    u8 flags;             // bit 0: PNG (otherwise BMP)
    u8 reserved;
    char name[256];      // zero-padded UTF-8 filename, max 255 bytes
};
```

## `book.bin`

### Version 9

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 9 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 9
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `reader_settings.bin`

### Version 9

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. Version 3 adds
the EPUB word-spacing level to that snapshot. Version 4 adds the EPUB indexing
method (`0` = incremental, `1` = full section). Version 5 appends a per-book
dictionary SD-font family name. Version 6 stores reader font sizes as physical
point sizes, version 7 appends the dictionary font's selected point size, and
version 8 splits the screen margin into vertical and horizontal values. Version
9 removes the obsolete per-book Dark Mode byte: Dark Mode is now a global
display setting.
This lets the
file preserve an auto-page-turn interval without forcing custom font/layout
settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Bionic Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 9
    u8 flags;   // bit 0 = custom reader settings, bit 1 = custom auto-page-turn interval, bit 2 = render mode override, bit 3 = dictionary font override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 readerFontPointSize; // physical point size; versions 2-5 stored a size slot
    u8 lineHeightPercent;
    u8 wordSpacing; // 0 = natural font spacing; 1-4 widen each gap by ~75% per level
    u8 orientation;
    u8 screenMarginVertical;
    u8 screenMarginHorizontal;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 bionicReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section
    char sdFontFamilyName[64];
    char dictionarySdFontFamilyName[64]; // meaningful only when flag bit 3 is set
    u8 dictionaryFontPointSize; // 0 = follow reader size
};
```

## `/.crosspoint/clippings/<bookType>_<crc32(path)>.bin`

### Versions 1-3

Clipping files store the per-book EPUB clipping list used by the reader. A
saved clipping is also what CrossInk renders as an in-reader highlight; there is
no separate highlight file. The file lives in `/.crosspoint/clippings/` instead
of the EPUB render-cache directory so clearing/rebuilding layout cache does not
delete user clippings.

The current implementation only writes EPUB clipping files, so `bookType` is
`epub`. The numeric suffix is `uzlib_crc32()` of the book's SD-card path, for
example:

```text
/.crosspoint/clippings/epub_1234567890.bin
```

Binary layout:

- `[0]` version (`1`, `2`, or current version `3`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `256`)
- book title (`String`)
- book author (`String`)
- book path (`String`)
- repeated clipping records:
  - `spineIndex` (`uint16_t` LE)
  - `startPage` (`uint16_t` LE)
  - `endPage` (`uint16_t` LE)
  - `pageCount` (`uint16_t` LE, at least `1`)
  - `startWordIndex` (`uint16_t` LE)
  - `endWordIndex` (`uint16_t` LE)
  - `wordCount` (`uint16_t` LE)
  - `paragraphIndex` (`uint16_t` LE, `UINT16_MAX` when unavailable)
  - `timestamp` (`uint32_t` LE, seconds since firmware boot when saved)
  - version 3 only: reader layout signature (`uint32_t` LE; font, spacing,
    viewport, and other section-layout inputs)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - version 1: selected text (`String`; legacy files were written with a
    `512`-byte in-app limit)
  - versions 2-3: selected-text length (`uint16_t` LE) followed by that many
    UTF-8 bytes (the current in-app limit is `4096` bytes, defined by
    `CLIPPING_TEXT_MAX`)

The clipping selector has a separate navigation bound: it exposes at most
`240` visible words from at most three pages. This is a bounded in-memory
selection window for low-memory devices, not a character-count limit. The
selected text is still stored separately and is limited to `4096` UTF-8 bytes.

CrossInk uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

Version 3 records which reader layout produced the numeric page/word anchor.
When that signature differs, CrossInk ignores the stale numeric range and
matches the saved text instead, including when both layouts happen to have the
same total page count. Versions 1-2 retain their numeric fast path until the
reader sees a relayout, when it stamps the previously active layout before
rebuilding.

Creating a clipping also appends a Kindle-style export entry to
`/My Clippings.txt` on the SD-card root. That text export can keep up to `2000`
bytes of the selected text and is append-only. Removing a clipping from the
reader deletes or rewrites only the binary clipping file; it does not remove
previous entries from `/My Clippings.txt`.

When CrossInk moves an EPUB through its built-in move-to-Read flow, it rewrites
the clipping file under the new path-derived name and removes the old one. If a
book is renamed or moved outside CrossInk, the path hash changes, so the old
clipping file may no longer be associated with the book until the file is moved
back or the clipping store is migrated.

## `stats_v5.bin`

### Version 5

`stats_v5.bin` stores per-book reading statistics for stats schema version 5.
Versioned filenames let firmware branches with different stats schemas keep
their own per-book stats files without overwriting each other. Version 5 extends
version 4 with a cached live reader book time-left estimate so Home and Reading
Stats can show the same estimate the reader last computed.

When `stats_v5.bin` is missing, CrossInk can read the previous versioned stats
filename (`stats_v4.bin` for version 5, `stats_v5.bin` after a future version 6
bump) before falling back to legacy `stats.bin` files with compatible stats
payloads. Future changes are always saved to the current versioned filename.

Binary layout:

- `[0]` version (`5`)
- `[1-2]` `sessionCount` (`uint16_t` LE)
- `[3-6]` `totalReadingSeconds` (`uint32_t` LE)
- `[7-10]` `totalPagesTurned` (`uint32_t` LE)
- `[11]` `isCompleted` (`uint8_t`)
- `[12-13]` `avgSecondsPerForwardPage` (`uint16_t` LE)
- `[14-15]` `paceSampleCount` (`uint16_t` LE)
- `[16]` flags (`bit0=startDateManual`, `bit1=finishedDateManual`)
- `[17-20]` `startDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[21-24]` `finishedDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[25-40]` `timeOfDaySeconds[4]` (`uint32_t` LE each)
- `[41-68]` `dayOfWeekSeconds[7]` (`uint32_t` LE each)
- `[69-72]` `estimatedTimeLeftSeconds` (`uint32_t` LE, `0` means unavailable)

## `section.bin`

### Version 66

Version 66 keeps the version 63 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout. Complete files use
version byte `66`, and suspended partials use sentinel byte `0xF6`.

### Version 62

Version 62 stores one compact source-whitespace bit per word in serialized text
blocks. Touch reader previews use it to reflow words with the selected font
without inferring spaces from device-specific pixel advances. Full and
suspended section caches rebuild together; complete files use version byte
`62`, and suspended partials use sentinel byte `0xF8`.

### Version 61

Version 61 is the v1.5.1 cache update. It stores `protectedImageUnits`
(`uint32_t` LE) after `pageCount`, so image-heavy sections estimate their
remaining non-image pages accurately. It also updates table fragments and
geometry, oversized-word wrapping, inline-image margins, and ruby continuation
layout. Full and suspended section caches rebuild together; complete files use
version byte `61`, and suspended partials use sentinel byte `0xF8`.

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 59 adds a compact page-start visible-text-offset lookup table. The
offset is a Unicode codepoint coordinate in the spine XHTML, so reader progress
and KOReader sync can return to the same content after a font, orientation, or
indexing-method change instead of relying on a page percentage. Suspended
incremental caches store the same table for their readable prefix; a target
beyond that prefix must continue indexing before it can be resolved.

Version 57 is binary-identical to version 56. The version was bumped because
word-gap suppression now applies only to tokens glued together in the source.
Older caches could collapse explicit spaces between Hangul words, so full and
suspended partial section caches rebuild together. Version 58 recalculates
Bionic Reading split-run offsets with the renderer's combined advance and
kerning rounding, so old cached page positions rebuild.

Version 56 changes `<br>` layout: a line break after text no longer reapplies
the containing block's top or bottom spacing, while an empty `<br>` block keeps
the existing scene-break gap. Full and suspended partial section caches rebuild
together. Version 55 assigns compact IDs to internal EPUB links. The ID is
stored in the existing per-word flags byte and in each page's footnote entry so
touch devices can map tapped text to the existing fragment-navigation path
without retaining another per-word data structure. Version 54 adds compact
ruby-text annotations to serialized text blocks. Only words that begin a ruby
group store annotation text; continuation words use a dedicated style bit. This
keeps books without ruby markup unchanged apart from the cache version while
avoiding an empty string allocation for every word.
Version 53 stores each image's EPUB-internal source path so section indexing can
read only its header and defer full extraction until the page is shown. Version
52 keeps Guide Dots centered when extra word spacing is enabled. Version 51
preserves continuation state for oversized CJK word fragments. Version 50
paginates chapter-heading image runs within the reader viewport so they do not
overflow into the reserved status-bar area. Version 49 stores Bionic Reading
split-run offsets in visual order so RTL word prefixes render on the right.
Version 48 changed Arabic contextual shaping and text measurement, so cached
word positions from version 47 no longer match what `drawText` renders.

Version 48 makes the EPUB word-spacing level widen the natural inter-word gap
(each level adds 10 pixels), which changes laid-out word positions, so
older sections must rebuild. Version 46 added the EPUB word-spacing level to the
cache-busting header. It retains the flat `TextBlock` arena and chapter-opener
anchor behavior introduced in version 45. It includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- visible-text-offset LUT used to resolve page positions across reflow and sync
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Bionic Reading and Guide Dots as per-word metadata instead of temporary layout words
- publisher CSS page-break handling and adjusted justification spacing baked into page layout
- table fragments
- per-page footnote entries
- per-page publisher page markers
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage: per-word arrays plus one shared NUL-terminated
  text blob, replacing length-prefixed word strings and parallel vectors. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 61
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasBionic;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasBionic != 0) {
            u16 wordBionicSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasBionic != 0) {
            u8 wordBionicBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        if (hasWordFlags != 0) {
            u8 wordFlags[wordCount] [[comment("bit 0 = black background, bit 1 = layout-inserted trailing hyphen")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String sourcePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
    u8 linkId;
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool bionicReadingEnabled;
    bool guideReadingEnabled;
    u8 wordSpacing;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 protectedImageUnits;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
