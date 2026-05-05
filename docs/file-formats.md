---
title: File Formats
nav_order: 8
---

# File Formats

## `book.bin`

### Version 3

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 3
#define MAX_STRING_LENGTH 65535

// === String Structure ===

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

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];
    
    // Metadata section
    Metadata metadata [[comment("Book metadata")]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];
    
    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 29

Added `PageTableFragment` page elements (tag `3`) for paginated simple-table layout. Table fragments store their fragment width, column count, cell padding, line height, per-row heights/header-divider flags, and per-cell serialized `TextBlock` lines. This invalidates cached section layouts so simple EPUB tables can render as buffered multi-column grids instead of flattened cell paragraphs.

### Version 27

Invalidates cached Bionic Reading layouts so words that were already bold in the EPUB remain fully bold. No binary layout fields changed from version 26.

### Version 26

Added `wordGuideDotXOffset` (uint16_t per word, appended after `wordBionicSuffixX` in the per-block data). Guide dot tokens (U+00B7, previously stored as individual TextBlock word entries) are now collapsed into the preceding word as a pixel offset annotation. Zero means no guide dot follows that word; non-zero is the offset from the word's x to where the dot is drawn. This eliminates ~N separate word entries per page when Guide Dots is active (where N is the inter-word gap count), reducing both cache size and deserialization time proportionally.

### Version 25

Added `wordBionicSuffixX` (uint16_t per word, appended after `wordBionicBoundary` in the per-block data). For words with `boundary > 0`, this stores the pixel offset from the word's x position to where the regular suffix begins - pre-computed at cache creation time from the layout x-position table. This eliminates a `getTextAdvanceX` call per bionic word from the render hot path (which ran twice per page turn). Zero when `boundary == 0`.

### Version 24

Bionic Reading words are now stored as a single merged TextBlock entry instead of split bold-prefix + regular-suffix token pairs. Each word gains a `wordBionicBoundary` byte (uint8_t per word, appended after `wordStyle` in the per-block data). A non-zero value N means the first N bytes of that word string are the bold prefix; the renderer applies the split at draw time. Zero means no split. This halves the token count for Bionic Reading pages, reducing serialized page size and deserialization time.

### Version 23

Added `guideReadingEnabled` (bool) to the header after `focusReadingEnabled`. Guide Dots feature flag: when enabled, a middle dot (U+00B7) is inserted between words during layout (skipped for Justify alignment).

### Version 22

Added `forceParagraphIndents` (bool) to the header after `extraParagraphSpacing`. This keeps cache invalidation aligned with the reader setting that synthesizes first-line indents when paragraph spacing is enabled.

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 29
#define MAX_STRING_LENGTH 65535
#define MAX_WORD_STRING_LENGTH 4096
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

// === String Structure ===

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

struct WordString {
    u32 length [[hidden, comment("Word byte length")]];
    if (length > MAX_WORD_STRING_LENGTH) {
        std::warning(std::format("Word length {} exceeds the firmware deserializer guard of {}", length, MAX_WORD_STRING_LENGTH));
    }
    char data[length] [[comment("UTF-8 word bytes")]];
} [[sealed, format("format_word_string"), comment("Length-prefixed UTF-8 word")]];

fn format_word_string(WordString s) {
    return s.data;
};

// === Page Structure ===

enum PageElementTag : u8 {
    PageLine = 1,
    PageImage = 2,
    PageTableFragment = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8
};

enum BlockStyle : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
};

struct TextBlock {
  u16 wordCount;
  WordString words[wordCount];
  s16 wordXPos[wordCount];
  WordStyle wordStyle[wordCount];
  u8 wordBionicBoundary[wordCount];
  u16 wordBionicSuffixX[wordCount];
  u16 wordGuideDotXOffset[wordCount];
  BlockStyle blockStyle;
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
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock textBlock [[inline]];
};

struct ImageBlock {
    String imagePath [[comment("Original cached image path")]];
    s16 width;
    s16 height;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock imageBlock [[inline]];
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount] [[inline]];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount] [[inline]];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount] [[inline]];
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct PageElement {
    u8 pageElementType;
    if (pageElementType == 1) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == 2) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == 3) {
        PageTableFragment pageTableFragment [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];
    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount] [[inline]];
};

struct AnchorEntry {
    String anchor;
    u16 pageIndex;
};

// === Section Bin Structure ===

struct SectionBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    // Cache busting parameters
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
    u32 lutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;

    Page pages[pageCount] [[inline]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Per-page file offsets
    u32 pageLut[pageCount];

    // Anchor -> page lookup
    u32 anchorOffsetCheck = $;
    if (anchorOffsetCheck != anchorMapOffset) {
        std::warning(std::format("Anchor map offset mismatch: expected 0x{:X}, got 0x{:X}", anchorMapOffset, anchorOffsetCheck));
    }
    u16 anchorCount;
    AnchorEntry anchors[anchorCount] [[inline]];

    // Paragraph index -> page lookup
    u32 paragraphOffsetCheck = $;
    if (paragraphOffsetCheck != paragraphLutOffset) {
        std::warning(std::format("Paragraph LUT offset mismatch: expected 0x{:X}, got 0x{:X}", paragraphLutOffset, paragraphOffsetCheck));
    }
    u16 paragraphCount;
    u16 paragraphIndices[paragraphCount];
};

// === File Parsing ===

SectionBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
