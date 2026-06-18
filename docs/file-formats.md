# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## `book.bin`

### Version 7

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 7
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

### Version 2

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. This lets the
file preserve an auto-page-turn interval without forcing custom font/layout
settings for the book.

```c++
struct ReaderSettingsBin {
    u8 version; // 2
    u8 flags;   // bit 0 = custom reader settings, bit 1 = custom auto-page-turn interval
    u16 autoPageTurnSeconds;

    u8 fontFamily;
    u8 fontSize;
    u8 lineHeightPercent;
    u8 orientation;
    u8 screenMargin;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 readerDarkMode;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 bionicReadingEnabled;
    u8 guideReadingEnabled;
    char sdFontFamilyName[64];
};
```

## `/.crosspoint/clippings/<bookType>_<crc32(path)>.bin`

### Version 2

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

- `[0]` version (`2`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `64`)
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
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - selected text (`String`, truncated to `512` bytes for the in-app store)

CrossInk uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

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

### Version 40

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 40 includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, and Guide Dots
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- table fragments
- per-page footnote entries
- per-page publisher page markers

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 41
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
    String words[wordCount];
    s16 wordXPos[wordCount];
    WordStyle wordStyle[wordCount];

    u8 hasFocus;
    if (hasFocus != 0) {
        u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
    }

    u8 hasGuideDots;
    if (hasGuideDots != 0) {
        u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
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

    u16 pageCount;
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
