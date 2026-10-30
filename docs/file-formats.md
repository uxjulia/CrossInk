# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## Server reading streak

`/.crosspoint/server_streak_<source>.bin` caches the combined CrossPoint Sync
calendar. `<source>` is the MD5 of the server base URL, a newline, and sync
username; no password is stored. The version 1 payload is 99 bytes: version
(`uint8_t`), anchor day since 2000-01-01 (`uint32_t`), 92 history bytes (bit N is
anchor minus N days, LSB first), and longest streak (`uint16_t`). The shared
`SyncFiles` writer appends CRC32 for a total of 103 bytes, publishes through
`.tmp`, and retains `.bak` for interruption/corruption recovery.

This is a replaceable server snapshot. Streak displays combine it with local
history when Reading Stats sync is enabled for the matching account. It is never
merged into `global_stats.bin`, uploaded as device history, or exported through
Nearby. Existing stats/cache versions stay unchanged; no cache reset is needed.

## Bookmark file safety

Bookmark files under `/.crosspoint/bookmarks/` retain their existing binary
versions (2–5). No format bump or cache reset is required.

Writes go to `<file>.tmp`, are checked, synced and closed, then replace the
primary through the shared atomic-file publisher. `<file>.bak` retains the
previous primary until publication completes; reads recover it after an
interrupted publication. Header scans ignore temporary files and can recover
backup-only stores.

Home header scans reject title, author or book-path fields over 1024 bytes.
Direct loads stream unused metadata and compare the stored path in 64-byte
chunks. Record counts and remaining file bytes are checked before a fallible
record allocation. A failed complete load or legacy merge disables editing and
saving for that session, preserving both source files until a successful
reload. This also fails closed on corrupt canonical files rather than replacing
them with a potentially incomplete legacy subset.

## Settings: menu navigation

`menuNavigation` in `/.crosspoint/crossink-settings.json` selects Directional
(`0`, the default) or Classic (`1`) button navigation in global Settings and
the EPUB reader menu. This is a global preference, shared with the in-reader
Controls screen. Missing or invalid values retain the current value, which is
Directional on startup. The setting uses the existing JSON export/import and
support-preference paths; no binary cache change or cache reset is required.

## Settings: status bar visibility and text size

`statusBarTextSize` in the settings JSON selects Small (`0`, Inter 8), Medium
(`1`, Inter 10), or Large (`2`, Inter 12) for both reader status bars. Missing
values retain Small; invalid values fall back to the default. Larger sizes
reserve additional reading space through the existing layout dimensions, so
EPUB layout caches rebuild automatically when those dimensions change. No
binary cache format change or manual cache reset is required.

`displayStatusBarTextSize` uses the same three values independently for the
global header status lane, configured under Settings > Status Bar. Missing or
invalid values default to Small. Global header height and available menu space
follow this size; reader pagination is unaffected.

`readerStatusBars.top.hidden` and `readerStatusBars.bottom.hidden` are optional
booleans. Missing values default to `false`. Hiding a bar preserves all of its
slots and options, removes its text and progress lane from reader geometry, and
reflows EPUB/TXT around the saved text position. XTC preserves its fixed-layout
page and clears the displayed status strip. These fields round-trip through
settings export/import and the web status-bar editor.

`readerStatusBars.top.centered` and `readerStatusBars.bottom.centered` are
optional booleans (Center Status Bar). Missing values default to `false`. When
`true`, that bar draws every non-empty slot as one centered row in slot order.
`readerStatusBars.top.separator` and `readerStatusBars.bottom.separator` pick the
glyph between items: bullet `•` (`0`), en dash `–` (`1`), or `|` (`2`). Missing
or invalid values fall back to the bullet. Bar height and reader pagination are
unaffected. These fields round-trip through settings export/import and the web
status-bar editor.

`readerStatusBars.top.battery`, `readerStatusBars.bottom.battery`, and the root
`displayBatteryStyle` select how a Battery slot renders in that bar: Icon + %
(`0`), Icon Only (`1`), or % Only (`2`). Invalid values fall back to Icon + %.
When a key is missing, it is migrated once from the legacy
`hideBatteryPercentage` value (Never, In Reader, Always): Never keeps Icon + %
everywhere, In Reader uses Icon Only for reader bars only, and Always uses Icon
Only everywhere. `hideBatteryPercentage` is no longer written.

Word Spacing preserves saved values `0..4`. Values `5..8` encode levels `-1..-4`; sliders display levels `-4..4`. Negative levels reduce the natural word gap by 20% per step, retaining at least one pixel. Positive levels retain their existing 10-pixel increments. The byte layout and cache identity are unchanged.

## TXT `index.bin`

Version 5 adds signed `characterSpacing` (-5..5) after the source file size in the index header. A changed spacing value or older version rebuilds pagination around the stored text position.

## TXT `progress.bin`

Each TXT cache stores a six-byte progress record: a little-endian `u16` page
number followed by a little-endian `u32` source-file byte offset. The format is
unchanged. A save during font reindexing writes `0xFFFFFFFF` for an unknown
offset, retaining the page number and compatibility with older readers.

Reopening prefers an in-range offset to preserve text position when the status
bar geometry changed while the book was closed. Offset zero is valid only for
page zero; a zero offset with a later page, an out-of-range offset, or a legacy
short record falls back to the saved page number. The loader tolerates existing
two- and four-byte page records. The stored page is clamped to the rebuilt index.

## `epub_<hash>/links.bin`

The EPUB reader writes followed-link Back history on clean exit (Home, sleep,
or reader replacement for sync). The record has a one-byte depth (1–3), followed
by that many pairs of little-endian `u16` spine index and `u16` page number,
oldest first: 5, 9, or 13 bytes. Empty history removes the file. A transient
footnote preview resumes at its immediate origin and omits that final entry;
earlier full-section links remain in the record.

On open, the reader checks the exact length and each spine index, closes the
file, and deletes it before adopting the history. Malformed records are also
consumed. A later clean exit rewrites the current stack; an unclean shutdown
cannot revive history from a previous session. This new sidecar does not change
EPUB layout cache formats. As with in-memory Back history, changing font or
layout may shift the destination page.

## `/.crosspoint/home_carousel_cache_<index>.bin`

### Version 6

The v1.6.1 release normalizes development version 8 to version 6, one step
after v1.6.0. The new per-position filenames and artwork cache keys prevent
reuse of older combined snapshots.

Each Carousel position has a disposable snapshot containing only cover artwork,
titles and position dots. Progress, reading time, header, menu icons and button
hints are drawn live after restoration. Each file contains a `CarouselCacheHeader`
followed by one full framebuffer. The header's `frameCount` records the number of
recent books used to compose the artwork, rather than the number of stored frames.
The key tracks ordered book paths, titles, cover paths, thumbnail availability and
image polarity, rather than reading progress or statistics.

Frames are rendered and saved only when viewed; returning Home does not prepare
other positions in advance. The development version 7 combined
`home_carousel_cache.bin` is removed after the first successful write. Cache
regeneration is automatic; EPUB layout caches and reading history are unaffected.

## `/.crosspoint/ttf-rendering.json`

This user-owned JSON file stores only custom TTF families whose raster settings
differ from CrossInk's defaults. Each entry is keyed by the installed family name
and contains numeric hinting, raster, interpreter, weight, and slant choices plus
the stem-darkening toggle. Missing families use automatic hinting, grayscale
output, the default interpreter, and no outline adjustments. The file currently
keeps at most 24 modified family profiles to bound RAM use while settings are open.

## `/.crosspoint/sleep-image-index/<directory-hash>-{bmp,all}.idx`

### Version 2

`ImageFolderIndex` (`src/activities/boot_sleep/ImageFolderIndex.{h,cpp}`) keeps
a compact, rebuildable index per indexed folder. It backs both the sleep-image
folder and the boot-screen folder (`/bootscreen` or `/.bootscreen`); the
directory name predates the boot-screen use and was kept as-is so existing
sleep caches aren't orphaned by an unrelated rename. The index uses only one
fixed-size filename record at a time in RAM. `bmp` contains BMP files and `all`
contains BMP and PNG files (sleep's Page Overlay mode only; the boot screen is
BMP-only and always uses a `bmp` index). The `validated` header flag means BMP
headers were checked while rebuilding after a failed render.

Version 2 appends an ordered folder fingerprint to the packed header; version 1
indexes rebuild automatically. On each sleep-image selection, the firmware walks
the folder's direct children and fingerprints eligible filenames, file sizes and
modification times. Additions, deletions, renames, replacements and changes to
directory order refresh the index on the next selection, including direct SD
copies. This costs a metadata walk per sleep, without decoding every image.
Boot-screen selection retains its existing cached/random selection policy.

The index is disposable. File-transfer, file-browser and preferred-folder changes
invalidate indexes; USB Drive invalidates them before detaching the filesystem
for raw host access (normal filesystem access resumes after reboot).
Invalidation preserves the separate deck checkpoints for unchanged collections.
If index/selection I/O or allocation fails, sleep retains its older scan/random fallback;
the full-deck guarantee requires a writable cache and renderable images.

```c++
struct ImageFolderIndexHeader {
    char magic[4];       // "CSIX"
    u8 version;          // 2
    u8 flags;            // bit 0: BMP+PNG, bit 1: BMP headers validated
    u16 pathLength;
    u16 recordCount;
    u16 recordSize;      // sizeof(ImageFolderIndexRecord)
    u32 recordsOffset;   // sizeof(header) + pathLength
    u64 fingerprint;     // ordered eligible filename/size/modification-time hash
    char directory[pathLength];
};

struct ImageFolderIndexRecord {
    u16 nameLength;
    u8 flags;             // bit 0: PNG (otherwise BMP)
    u8 reserved;
    char name[256];      // zero-padded UTF-8 filename, max 255 bytes
};
```

### Sleep shuffled deck (`<index-path>.deck`), version 1

Sleep images are drawn uniformly from the remaining images, which is equivalent
to shuffling a deck and dealing every card once. A packed checkpoint stores one
bit per index record (least significant bit first; 1 = attempted/skipped) plus this header:

```c++
struct SleepImageDeckHeader {
    char magic[4];       // "CSDK"
    u8 version;          // 1
    u64 fingerprint;     // index fingerprint
    u16 count;           // number of indexed candidates
    u16 lastIndex;       // last displayed image, or 65535 before any display
    u8 hasDisplayed;     // a successful display in the current round (0 or 1)
    u64 checksum;        // FNV-1a over preceding header bytes and bitmap
    u8 drawn[(count + 7) / 8];
};
```

Numeric fields are little-endian, without padding. The checkpoint survives deep
sleep and restart. Once all cards are drawn the bitmap clears; with more than one
eligible image, the first successful display excludes the previous round's last
displayed image, even if earlier attempts in the new round failed. Changes to the
collection start a fresh deck; normal BMP and Page Overlay collections have
independent checkpoints. Hidden files and subfolders are excluded, as before.
Sleep retries retain the same index positions and deck: BMP validation and the
BMP-only retry after a PNG failure filter the remaining cards, without resetting
the round. Validation skips all known-malformed BMPs for the round; temporary
BMP-only filtering leaves untried PNGs available. An exhausted retry subset does
not bypass the deck through a random
scan. The existing root/default screen remains available when no retry can draw.

The bitmap needs `ceil(count / 8)` bytes of temporary heap (125 bytes for 1,000
images, capped at 8,192 bytes for 65,535 images), freed before rendering. Retry
filtering temporarily uses a second bitmap of the same size. After each render
attempt, the drawn bit is saved; only a successful render updates the last
displayed index. This consumes failed images without falsely counting them as
displayed for boundary protection. Each attempt writes a synced temporary
checkpoint and publishes it through the shared atomic
file helper, retaining `.bak` until publication succeeds. A remaining backup is
recovered before reading; a malformed or checksum-invalid checkpoint starts a
fresh deck. Deleting `/.crosspoint/sleep-image-index/` resets indexes and decks.

## `/.crosspoint/library.idx`

### Version 8

Version 8 keeps the version 7 layout and rebuilds the index to exclude
`XTCache` and `System Volume Information` directories, regardless of FAT name
casing. Version 7 remains readable for reconciliation, preserving arrival
history and reusable metadata. Rebuilding never deletes the source books or
system directories.

### Version 7

Version 7 preserves the version 6 layout but invalidates extracted metadata to
resolve EPUB3 series refinements by collection ID. The next Library scan reparses
EPUB metadata when enabled, preserving first-seen history during reconciliation.
The persistent metadata cache is also invalidated, so it cannot restore stale series.

### Version 6

Each book's name blob now ends with a `uint32_t` series position after the
length-prefixed genre. The value is a sortable encoding of a finite signed
IEEE-754 single-precision number; `0xFFFFFFFF` means missing or invalid. Series
sorting compares folded series name, then this position, then title order. Missing
positions follow numbered books within a series. Calibre `series_index` and
EPUB 3 `group-position` supply the value. Version 5 and earlier indexes rebuild
on the next Library scan; EPUB metadata is reread when enabled to obtain the new
field, while `firstSeen` history survives reconciliation.

### Version 5

Date Added now uses the filesystem creation timestamp. A title-ordered array of
`uint32_t` packed FAT creation date/times follows the five `uint16_t` sort
permutations and precedes the aligned name section. A zero value means the
creation timestamp is unavailable; `firstSeen` orders books with equal or
missing timestamps, and zero sorts before dated values in ascending order. The
128-byte book record still stores modification time
separately for EPUB metadata freshness. Version 4 indexes rebuild on the next
Library scan, retaining metadata and `firstSeen` values from the old index.
Header flag bit 2 marks an arrival order that fell back to `firstSeen` because
sorting by creation time ran out of memory; the next scan retries it. Version 4
uses only bits 0 and 1, so its flags remain valid during reconciliation.

### Version 4

Two more `uint16_t` permutations follow arrival order: series order and genre
order. Both use folded EPUB metadata, place missing values last in ascending
order, and use title order to break ties. The header, records, and name blobs
remain compatible with version 3. During the one-time rebuild, version 3
metadata and `firstSeen` arrival history are reused for unchanged books.

### Version 3

The Library index adds two length-prefixed fields after source author in each
book's name blob: series and genre. The fixed-size header and record layout stay
the same. A version 2 index is read once during rebuilding so existing
`firstSeen` values survive; the new index is then written as version 3.
Series and genre are read only from EPUB metadata. The library treats the first
`dc:subject` value as genre, and reads Calibre or EPUB 3 series metadata.

### Version 2

Version 2 added EPUB title and author metadata, the file modification time,
and source-author spelling to the original Library index. Its header and
128-byte records are compatible with version 3 reconciliation; its name blobs
do not contain series or genre.

### Version 1

`LibraryIndexFile` (`lib/LibraryIndex/LibraryIndexFile.{h,cpp}`) reads the
`CLX1` on-disk index for the Library screen: one sorted, searchable
snapshot of up to 32,767 books on the card, built by `LibraryBuilder` so paging,
sorting, and searching the shelf cost a handful of seeks instead of a
directory walk per screen. The format itself (`lib/LibraryIndex/LibraryFormat.h`)
is free of `HalStorage` and Arduino so its layout and validation rules are
host-testable (`test/library_format`, `test/library_index_file`).
Builds keep RAM flat on every device: each sort holds a fixed buffer and spills
sorted runs to the card when a library outgrows it, and the previous index is
matched through a sorted file rather than an in-RAM table. While building, the
transient files `library.stage`, `library.stage.f`, `library.prior`,
`library.rename`, `library.order`, `library.authors`, `library.canon`, and
`library.runs` live in `/.crosspoint`; every build removes them when it ends. If
the scan finds another book beyond the limit, the rebuild fails and keeps the
previous index instead of publishing a partial shelf.

Every section starts on a 512-byte boundary. Records are a fixed 128 bytes
each, so record `k` always lives at `recordStart + 128*k` with no offset table
to load first, and 32 records exactly fill a 4096-byte scan buffer. Five
`uint16_t` permutation arrays (surname, first name, arrival, series, then genre
order) and one `uint32_t` creation-time array let those sorts page without
re-sorting on every open;
Title order needs no permutation because the record section is already
title-sorted.

The index is disposable: a bad magic, an unknown format/fold version, a size
mismatch (a build interrupted by power loss cannot pass, since `selfSize` is
checked against the real file size), or an inconsistent section layout all
cause a full rebuild rather than a crash or silently wrong output. A fold
version bump alone (the text-normalisation rules changed) can be handled by
reconciliation instead: `openForReconciliation()` accepts stale sort/search
keys so each book's `firstSeen` arrival order survives across the rebuild
even though its fold and permutations are regenerated.

CrossInk's format version is `6`; older indexes rebuild automatically. Versions
2 through 5 can be read for reconciliation so arrival history survives. The fold
version is `3`.

```c++
struct ClixHeader {            // 64 bytes, padded to the first 512-byte sector
    char magic[4];              // "CLX1"
    u8 formatVersion;           // 6
    u8 foldVersion;             // 3
    u8 flags;                   // bit0: ranks degraded, bit1: dedup degraded, bit2: arrival degraded
    u8 metadataEnabled;         // 0 or 1
    u16 bookCount;
    u16 folderCount;
    u16 nextFirstSeen;
    u16 padding1;
    u32 folderStart;
    u32 folderLen;
    u32 recordStart;
    u32 permStart;
    u32 nameStart;
    u32 nameLen;
    u32 selfSize;                // expected total file size; truncation guard
    u8 reserved[20];
};

struct ClixRecord {             // exactly 128 bytes; record k @ recordStart + 128*k
    u32 nameOff;                 // offset into the name blob, from nameStart
    u32 fileSize;                // captured while the dirent was open
    u16 firstSeen;
    u16 folderId;                // index into the folder table
    u8 nameLen;
    u8 foldLen;
    u8 authorKeyLen;
    u8 metadataStatus;           // 0 not attempted, 1 extracted, 2 failed
    char fold[96];               // folded sort/search key
    char authorKey[12];          // order-insensitive author identity
    u32 modificationTime;        // packed FAT date/time, or 0 if unavailable
};

struct ClixFolderHeader {        // one per indexed folder, back to back
    u8 pathLen;                  // 1..255; path bytes follow, no trailing '/'
};
```

The name blob for each record (found via `nameOff` into the `names` section)
holds, back to back: an 8-byte FNV-1a path hash of the book's complete path
(the identity used by rebuild reconciliation and by "is this book already in
the index" lookups), the filename, then five length-prefixed fields —
display author, title, the pre-spelling-harmonisation source author, series,
and genre. Version 6 appends the four-byte series position.

## `/.crosspoint/library.meta` and `/.crosspoint/library.metd`

### Version 2

Version 2 keeps the binary layout unchanged and rejects version 1 extraction
results so library scans rebuild series data using ID-based EPUB3 resolution.

Collection resolution accepts up to eight exact IDs in a shared 2048-byte arena;
oversized IDs and candidates exceeding the budget are ignored rather than joined
to the wrong title or position. Text fields retain the 512-byte UTF-8-safe limit,
Calibre series metadata takes precedence, and parser indexes remain strings.
The reader's `book.bin` version is unchanged: it stores no series fields, and
`Epub::loadMetadata` already bypasses it when series metadata is requested.

### Version 1

`LibraryMetadataCache` (`lib/LibraryIndex/LibraryMetadataCache.{h,cpp}`) keeps
EPUB metadata from the moment each book is parsed, independent of whether the
Library build that parsed it finishes. A later build looks a book up only when
the previous `library.idx` cannot supply reusable metadata, so a cancelled or
failed scan resumes without re-parsing what it already read. Failed parses are
not stored. A book is identified by its complete-path FNV-1a hash (the same
`clixPathHash` the index uses), file size, and packed FAT modification time;
books with no modification time are never cached.

`library.meta` holds a 32-byte header padded to 512 bytes, then 65,536 16-byte
slots of an open-addressing hash table (linear probing, at most 64 probes). An
all-zero slot is empty. A slot whose check does not match is skipped. There is
one slot per path; storing a changed book replaces its slot's payload offset.

`library.metd` is append-only. Each record is a 32-byte header followed by
title, author, series, and genre bytes (each at most 255 bytes, cut at a UTF-8
boundary). Payloads are written before the slot that points at them, and every
read verifies the record checksum, so a torn write costs a re-parse rather than
wrong metadata. The cache is discarded and recreated when the header does not
validate, more than 75% of slots are used, or the payload passes 64 MiB. Bump
the cache version whenever the set of extracted metadata fields changes.

```c++
struct CacheHeader {             // 32 bytes at offset 0
    char magic[4];               // "CLM1"
    u8 version;                  // 1
    u8 padding[3];
    u32 slotCount;               // 65536
    u32 usedSlots;               // slots ever claimed; written on close
    u8 reserved[16];
};

struct CacheSlot {               // slot i @ 512 + 16*i
    u64 pathHash;
    u32 payloadOffset;           // into library.metd
    u32 check;                   // FNV-1a of pathHash and payloadOffset, low bit set
};

struct PayloadHeader {           // 32 bytes, then the four strings back to back
    u32 magic;                   // "CLMP"
    u64 pathHash;
    u32 fileSize;
    u32 modificationTime;
    u32 seriesPosition;          // same encoding as library.idx version 6
    u8 titleLen;
    u8 authorLen;
    u8 seriesLen;
    u8 genreLen;
    u32 checksum;                // FNV-1a of this header (checksum zeroed) and the strings
};
```

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 10 rebuilds metadata with namespace-aware OPF parsing so optimizer-generated
XML prefixes do not leave an empty chapter list. The binary layout is unchanged.
Version 9 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
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

### Version 13

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
display setting. Version 10 appends a field mask so a book overrides only the
reader settings that differ from its current global defaults. Version 2-9
records with the custom-settings flag keep their full snapshot as an override
when migrated; they cannot distinguish past manual edits from automatic ones.
Version 11 appends the image-grayscale toggle after the mask and moves the SD-font
mask bit from 18 to 19. Older records retain their existing overrides and inherit
the global image-grayscale setting. This drawing-only setting does not invalidate
EPUB layout or image caches.
Version 13 appends `u8 publisherFonts` (0 Off, 1 Ignore Body Font, 2 All Text), with override bit 21. Versions 1–12 retain their previous fields and inherit the global publisher-font mode.

Version 12 appends `u8 characterSpacing` (0–10 encodes -5–+5, default 5), with override bit 20. Existing field bits, including the SD font at bit 19, stay unchanged. Older records inherit global character spacing. Each displayed step adjusts inter-character gaps by half a pixel; spaces keep their word-spacing behavior.

The file can preserve an auto-page-turn interval without forcing custom
font/layout settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Focus Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 12
    u8 flags;   // bit 0 = at least one custom reader field, bit 1 = custom auto-page-turn interval, bit 2 = render mode override, bit 3 = dictionary font override, bit 4 = Safe Mode override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 readerFontPointSize; // physical point size; versions 2-5 stored a size slot
    u8 lineHeightPercent;
    u8 wordSpacing; // 0 = natural; 1-4 add 10px per level; 5-8 encode -1..-4 (20% tighter per level)
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
    u8 focusReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section
    char sdFontFamilyName[64];
    char dictionarySdFontFamilyName[64]; // meaningful only when flag bit 3 is set
    u8 dictionaryFontPointSize; // 0 = follow reader size
    u32 readerSettingsOverrideMask; // bits 0-17 = snapshot fields excluding snapshotRenderMode; bit 18 = imageGrayscale; bit 19 = sdFontFamilyName
    u8 imageGrayscale; // 0 = BW only, 1 = grayscale (default)
    u8 characterSpacing; // 0..10 encodes -5..+5; override bit 20
};
```

## Sync files

`sync_pending.bin`, `sync_state.bin` and each book's `sync.bin` share one
crash-safe pattern (`src/sync/SyncFiles.cpp`). A change writes `<name>.tmp`,
closes it, renames the current file to `<name>.bak`, then renames the temp file
into place. When there is no current file (first write, or recovering from the
backup) the temp file is renamed straight into place and `.bak` is kept. Every
file ends with a little-endian `uint32` CRC32 (`uzlib_crc32`) of all bytes
before it. A reader checks the CRC first. If the main file is missing but a
CRC-valid `.tmp` exists (power lost between the two renames), that newest copy
is renamed into place and used; otherwise it falls back to `.bak`. If nothing
valid remains the file starts fresh (see below). Integers are little-endian.

### `/.crosspoint/sync_pending.bin` (version 1)

Books whose local state changed since their last successful upload.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| entries | repeated | until the CRC |
| entry.mask | `uint8` | `1` progress, `2` reading stats, `4` clippings |
| entry.path | `uint16` length + bytes | absolute book path, at most 1024 bytes |
| crc | `uint32` | |

Entries are only added while a sync account exists, and stats/clippings bits
only while their What to Sync toggle is on. An entry is removed when its mask is
empty. The file is never deleted; an empty list is just the version byte. If it
and its backup are both unreadable after a catch-up has completed, the catch-ups
in `sync_state.bin` are reset so they run again.

### `/.crosspoint/sync_state.bin` (version 2)

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| baseUrl | `uint16` length + bytes | account this state belongs to |
| username | `uint16` length + bytes | |
| flags | `uint8` | bit 0 stats caught up, bit 1 clippings caught up |
| statsCatchupId | `uint32` | non-zero while a stats catch-up is unfinished |
| clippingsCatchupId | `uint32` | same for clippings |
| nextStampId | `uint32` | next catch-up/walk id; random start, never reused |
| scopes | repeated | version 2: prepared sync scopes, until the CRC |
| scope.path | `uint16` length + bytes | folder, or `/` for the whole card; at most 255 bytes |
| scope.flags | `uint8` | bit 0 prepared, bit 1 stats caught up, bit 2 clippings caught up |
| scope.cursor | `uint32` | change feed cursor for this scope |
| scope.lastSynced | `uint32` | stamp id of the last sync; the oldest is dropped beyond 32 scopes |
| scope.walkId | `uint32` | non-zero while the scope's first walk is unfinished |
| scope.statsCatchupId | `uint32` | unfinished stats catch-up for this scope |
| scope.clippingsCatchupId | `uint32` | unfinished clippings catch-up for this scope |
| crc | `uint32` | |

Version 1 files (no scopes) are read and rewritten as version 2. A different
base URL or username reads as a fresh state (catch-ups and first walks run
again). Scopes are streamed one at a time, so RAM use does not grow with the
list.

### `/.crosspoint/sync_ids.bin` (version 1)

Document id -> book path for books with saved progress, stats or clippings on
this reader, read by the progress pull. XTC books and finished books are never
recorded. Unlike the other sync files it is appended to, so each record carries
its own CRC and there is no trailing file CRC; rewrites (rename, removal,
compaction) still use the `.tmp` swap. A confirmed Document Matching change
deletes it, together with `sync_parked.bin` and every scope's cursor, first walk
and catch-ups in `sync_state.bin`.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| method | `uint8` | `DocumentMatchMethod`; another method reads as an empty map |
| generation | `uint32` | changes whenever the file is created anew, compacted, or rebuilt after a corrupt entry |
| headerCrc | `uint32` | CRC32 of the 6 bytes above |
| entry.id | 16 bytes | raw KOReader document MD5 |
| entry.path | `uint8` length + bytes | absolute book path, at most 255 bytes |
| entry.crc | `uint32` | CRC32 of the entry's id, length and path |

A torn append only loses that entry: the next record finds the partial tail and
rewrites the file before appending. If the main file is missing after a crash
between the two renames, a `.tmp` with a valid header is promoted, else the
`.bak`. Copies of a file in binary mode share an id, so an id can map to several
paths.

### `/.crosspoint/sync_parked.bin` (slot version 1)

Remote positions from the change feed that the pull could not apply: books not
on this reader (or not in `sync_ids.bin`), books never opened here, failed
applies, and Ask on conflicts' Skip book. At most 32 fixed 384-byte slots; a
change sharing a hash with a parked one replaces it in place, else the first
empty slot is used, else the oldest (by remote timestamp) is overwritten. A slot
whose CRC does not match (torn write, or zeroed by a removal) is empty. There is
no file header or file CRC. Removing the last valid entry deletes the file, so
opening a book with nothing kept stops at the missing file.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | version | `uint8` | `1`; `0` = empty slot |
| 1 | hashCount | `uint8` | 1-4 |
| 2 | flags | `uint8` | bit 0: rich position present |
| 3 | reserved | `uint8` | |
| 4 | hashes | 4 x 16 bytes | raw document MD5s: canonical first, then aliases |
| 68 | percentage | `float` | |
| 72 | timestamp | `int64` | remote write time |
| 80 | pctQ | `uint32` | rich position |
| 84 | spine, page, pages, paragraph | 4 x `uint16` | rich position; paragraph 0 = none |
| 92 | device | 32 bytes | remote device name, NUL-padded |
| 124 | xpath length | `uint16` | at most 252; longer positions are not parked |
| 126 | xpath | 252 bytes | |
| 380 | crc | `uint32` | CRC32 of bytes 0-379 |

### `<book cache>/sync.bin` (version 2)

In the book's `epub_<hash>/` or `xtc_<hash>/` folder; only written when that
folder already exists.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| hasId | `uint8` | `1` when `id` is valid |
| method | `uint8` | `DocumentMatchMethod` the id was computed with |
| id | 16 bytes | raw KOReader document MD5 |
| fileSize | `uint32` | book size when `id` was computed; a different size recomputes it |
| statsStamp | `uint32` | stats catch-up id that last sent this book |
| clippingsStamp | `uint32` | clippings catch-up id that last sent this book |
| walkStamp | `uint32` | version 2: first-walk id that finished this book |
| idsGeneration | `uint32` | version 2: `sync_ids.bin` generation the book was recorded in; 0 = not recorded |
| crc | `uint32` | |

Version 1 records are read with both new fields 0.

Cache clears preserve `sync.bin` and its `.bak`/`.tmp` recovery files. File
replacements clear the cached document id and map generation in both the main
record and backup, retaining the catch-up and first-walk stamps. Saved data is
queued for sync again; the next document lookup recalculates the id even when
the replacement has the same file size. No binary format version changes.

## `/.crosspoint/clippings/<bookType>_<crc32(path)>.bin`

### Versions 1-4

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

- `[0]` version (`1`, `2`, `3`, or current version `4`)
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
  - versions 3-4: reader layout signature (`uint32_t` LE; font, spacing,
    viewport, and other section-layout inputs)
  - version 4: table selection (`uint16_t` LE; `UINT16_MAX` for non-table text)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - version 1: selected text (`String`; legacy files were written with a
    `512`-byte in-app limit)
  - versions 2-4: selected-text length (`uint16_t` LE) followed by that many
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
same total page count. Legacy records without a layout signature use text
matching rather than trusting ambiguous numeric ranges. Version 4 adds the
table selection field; versions 1-3 remain readable.

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

## `stats_v6.bin`

### Version 6

Version 6 extends the 73-byte version 5 layout below to 75 bytes:

- `[0]` version (`6`)
- `[1-72]` unchanged version 5 fields
- `[73-74]` `paceFractionMillis` (`uint16_t` LE, `0..999`)

Reading pace is `avgSecondsPerForwardPage * 1000 + paceFractionMillis`
milliseconds per forward page, capped at `65535000` ms. The fraction is zero
at that cap. Fractional precision is preserved across updates and reopen.
The whole-second field and sync payload retain their existing units.

When absent, reads try `stats_v5.bin`, `stats_v4.bin`, then legacy `stats.bin`.
Older payloads start with a zero fractional part. Saves write only version 6;
older canonical snapshots remain available to older firmware. Reset removes
all supported versioned snapshots and recovery files to avoid restoring old
history. This stats-only change does not invalidate EPUB layout caches.

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

### Version 102

Complete caches use `102`; suspended partial caches use `0xD7`. GIF images now
participate in EPUB layout, so older pages must be rebuilt. The binary layout
is unchanged.

### Version 101

Complete caches use `101`; suspended partial caches use `0xD6`. SD-font layout
rounds each glyph advance to pixels before summing, matching drawing while
preserving character spacing and boundary kerning. The payload is unchanged.
Both complete and partial caches from version `100` / `0xD5` and earlier rebuild
automatically; saved reading progress and book metadata are preserved.

### Version 100

Complete caches use `100`; suspended partial caches use `0xD5`. The payload
retains the reference-text offset tables introduced in version 99 below.
Layout beside drop caps stops after the requested lines, leaving the remaining
words intact for full-width layout. Both cache types rebuild to remove stale
hyphens and refresh wrapping; book metadata and saved progress are kept.
Version `99` / `0xD4` and earlier caches are rejected as stale.

### Version 99

Complete caches use `99`; suspended partial caches use `0xD4`. The header and
existing lookup-table offsets are unchanged. After the visible-text offset
table, a second `uint32_t[pageCount]` table stores normalized reference-text
character offsets at each page start, followed by a `uint32_t` total normalized
text length (zero while partial). The partial parse-watermark trailer follows
these new fields. Both cache types rebuild automatically.

All section builders record these coordinates, including sleep-page rebuilds
that skip location metadata. The in-memory page index grows from 12 to 16 bytes
per page, in fallible 1 KiB chunks; active text buffers also retain the parser's
existing optional four-byte reference offset per buffered word.

These coordinates let book time-left use the stable-page manifest's text units
without relying on a provisional chapter page count. Character-based manifests
work during incremental indexing. Older word-based manifests use the complete
section's text length to map character positions to fractional word progress;
they retain the screen-page fallback while that length is unknown. Stable pace
is learned in memory from three accepted forward reads per session and remains
valid through font/layout changes; the persisted screen-page pace is separate.

### Version 98

Complete caches use `98`; suspended partial caches use `0xD3`. The payload is
unchanged. Pending words are committed to their original paragraph before a
nested block changes layout or inherited font styles. Both cache types rebuild
automatically so existing books pick up the corrected word placement (#886).

### Version 95

Complete caches use `95`; suspended partial caches use `0xD0`. The payload is
unchanged. Image-only paragraphs retain publisher alignment independently of
the reader's prose alignment. Both cache types rebuild to refresh image positions.

### Version 94

Complete caches use `94`; suspended partial caches use `0xCF`. The payload is
unchanged. Fieldset containers now have a default outline and padding, with
publisher CSS taking precedence. Both cache types rebuild to apply the updated
wrapping, box decorations, and pagination.

### Version 93

Complete caches use `93`; suspended caches use `0xCE`. After the render-mode
byte, the header adds `u8 publisherFonts`, `u32 publisherFontSignature` (face
manifest identity), `u32 publisherFontFaces` (requested face bitmask), and
`u32 publisherFaceSignature` (loaded face fingerprints and fallback results),
and `u32 publisherLoadedFaces` (successfully admitted faces). Reopening gives
previously successful faces priority before retrying fallback faces.
The face state is captured after page layout and patched when the complete or
partial cache is committed. Reopening restores only requested families and
rejects cached layout if their availability or fingerprints differ.
Text blocks add a boolean byte after their packed arena, followed by one
`u32` CSS family hash per word when present, before the ruby count. A zero
family keeps the reader font. Drop caps add a `u32` family hash before their text length. All older complete and partial caches rebuild.

CSS cache revision `23` adds a four-byte family hash to each style (96 bytes
total); defined bit 29 marks an explicit family. After the descendant rules,
the same CSS cache contains an 8-bit face count (maximum 32). Each face stores
a 32-bit family hash, style flags (bit 0 bold, bit 1 italic), an 8-bit path
length and that many UTF-8 path bytes. Paths are relative to the EPUB root.
A final `u32` points to the face count for direct font-loader access. Empty
manifests still store the count and footer. Cache validation checks this tail;
rules and font metadata share the existing atomic cache replacement.


### Version 91

Complete caches use `91`; suspended partial caches use `0xCC`. The payload is
unchanged. Figure containers now apply publisher alignment and spacing, so both
cache types rebuild to center chapter ornaments and separate them from nearby text.

### Version 90

Complete caches use `90`; suspended partial caches use `0xCB`. The payload is
unchanged. ESP32-S3 readers now apply installed bitmap sizes to EPUB text, so
both cache types rebuild to use the updated wrapping and line heights.

### Version 89

Complete caches use `89`; suspended partial caches use `0xCA`. The payload is
unchanged, but C3 EPUB headings and CSS font sizes now use available physical
bitmap sizes. Both cache types rebuild to pick up the new wrapping and line
heights. SD reader font identities include the content of the optional sizes
that fit the memory budget, so a changed size file or a load under tighter
memory also invalidates previously paginated sections.

### Version 88

Complete caches use `88`; suspended partial caches use `0xC9`. Both rebuild older caches automatically. The section header adds signed `characterSpacing` (-5..5) immediately after `wordSpacing`. Each TextBlock adds the same signed byte immediately before `initialLetterBytes`, so cached drawing retains the exact spacing used for layout. No additional per-word allocation is needed.

### Version 87

Publisher decorations, contextual CSS selectors, preserved whitespace and inline
font sizes change layout. Complete caches use `87`; suspended partial caches use
`0xC8`. Version `86` / `0xC7` already exist on another local feature branch and
are deliberately not reused. Both old full and partial caches rebuild.

TextBlock adds one-byte `wordSizesPresent` and `initialLetterBytes` fields immediately before `textBytes`.
The initial-letter prefix (0–12 bytes) stays in the first logical word for dictionary lookup and clippings; ordinary text rendering skips those bytes because the drop cap draws them separately.
When `wordSizesPresent` is set, the arena stores one point-size byte per word after the whitespace
bitset and before UTF-8 text; zero uses the block font. The flag is zero for
ordinary lines, so they require no extra per-word storage. Sizes use the existing
scalable font family and share the tallest word's baseline and line height.

Page tags `5` and `6` are drop caps and border boxes. Drop caps contain x/y
(int16 each), source font point size (uint8, zero means the reader font), Q8
scale (uint16), style (uint8), UTF-8 byte length (uint8, 1–12), then text bytes.
Border boxes contain x/y (int16 each), width/height (uint16 each), four pairs of
width/style bytes in top/right/bottom/left order, and a shaded byte. Styles are
none/solid/double/dotted/dashed (0–4), and border widths are capped at 8 pixels.

CSS cache revision `22` grows the fixed style payload from 80 to 92 bytes:
whitespace byte after the four existing border masks, eight border width/style
bytes, then shaded/float-left/initial-letter bytes, followed by the existing
uint32 defined flags. New flag bits 25–28 identify whitespace, shading, float
and initial-letter declarations. Context-rule records retain the two-string
format; the first string is the ancestor chain (`@` means no constraint), and
the second is the subject compound, optionally ending in `::first-letter`.

### Version 86

Complete sections use byte `86` and suspended partials use `0xC7`. A little-endian
`u32` hyphenation identity follows `hyphenationEnabled` in the header. It is zero
when disabled or unavailable, one for built-in English, and a fingerprint of the
external pack's language, prefix/suffix rules, root offset, size and payload CRC
otherwise. Storage offsets are excluded. A mismatch rebuilds the section,
including suspended incremental builds; older full and partial versions rebuild.

### Version 85

Long paragraphs apply initial CSS margin/padding on their first emitted line,
including incremental flushes. Text fragment bounds are checked on each append.
Both branch-specific version 85 formats are invalidated to rebuild complete and
suspended layouts with the combined header and paragraph-spacing rules.

### Version 84

Explicit CSS border suppression on `<hr>` removes the generated rule from page
layout. The section payload is unchanged; complete files use byte `84` and
suspended partials use `0xC5`. Older full and partial caches rebuild automatically.

CSS cache revision `21` appends four edge masks after `listStyleType`: suppressed
styles, zero widths, defined styles, and defined widths (one byte each, with bits
0-3 for top/right/bottom/left). Defined-property bit 24 marks border state. The
fixed style payload grows from 76 to 80 bytes. Older CSS caches rebuild.

### Version 83

Nested paragraphs and other blocks retain inherited CSS bold and italic styles,
including explicit child overrides. The payload is unchanged from version 82,
but glyph styles and wrapping can differ. Complete files use byte `83`;
suspended partials use `0xC4`. Older full and partial caches rebuild automatically
so previously cached regular text does not hide the corrected styling. The CSS
rule cache format is unchanged.

### Version 82

Scalable-font EPUB headings and whole text blocks carry a resolved point size
and line height. Each serialized `TextBlock` appends `u8 fontSize` (0 = reader
font, otherwise 8-44 pt) and `u16 lineHeight` after `directionDefined` in its
`BlockStyle` payload. The inherited Q8 font scale is layout-only and is not
serialized. Complete section files use byte `82`; suspended partials use `0xC3`.
Older full and partial caches rebuild automatically.

CSS cache revision `20` adds a five-byte font-size length (float value plus unit)
after `imageWidth` and uses defined-property bit 23. The fixed style payload is
76 bytes. Older CSS caches rebuild automatically.

On scalable fonts, headings default to 2, 1.5, 1.17, 1, 0.83, and 0.67 times the
inherited size, rounded to whole points and bounded to 8-44 pt. Enabled book
styles can override block sizes using em, rem, %, px, pt, and size keywords.
CSS 16px/12pt maps to the user's selected body size; em/% use the parent and
rem uses the HTML root. Inline span size changes and table-cell sizing remain
uniform in this phase. Light mode and bitmap fonts retain their existing sizes.

### Version 81

Version 81 carries `text-indent` from the HTML and body root styles into
descendant paragraph blocks. Existing full section caches (byte `80`) and
suspended partial caches (`0xC1`) rebuild so inherited paragraph indentation is
reflected in saved page positions. Complete files use byte `81`; suspended
partials use marker `0xC2`. The CSS rule cache format is unchanged.

### Version 80

Version 80 places small inline images within text lines while keeping larger
images as centered blocks. Page-image records add a one-byte inline flag after
their coordinates; older full and suspended partial section caches rebuild so
existing books receive the new layout. The CSS rule cache moves to version `19`
so `display: inline` rules retain their meaning.

The v1.6.1 release candidate used version `80` and partial marker `0xC1`: development
versions `78` and `79` used the older image payload, and earlier release
preparation used partial marker `0x80`. Reusing those identifiers could accept
incompatible saved pages. Version `81` and marker `0xC2` added the root-style
indentation change; the final v1.6.1 release uses version `83` and marker `0xC4`
for the further layout changes described above. All of those older caches rebuild
automatically.

### Version 79

Version 79 keeps the version 78 serialized layout. Korean words now wrap at
spaces by default; with hyphenation enabled, they can also split at a legal
CJK boundary at a line end without a visible hyphen. Justification stretches
word spaces only. Full caches (byte `79`) and suspended partial caches
(`0xF4`) both rebuild because earlier page positions are no longer valid.

### Version 78

Version 78 changed layout for inline CSS padding. Full and suspended partial
section caches rebuild together.

### Version 77

Version 77 keeps the serialized layout unchanged. It was bumped because ordered
lists now number their items, `list-style-type: none` suppresses markers, and
`<ul>`/`<ol>` margins and padding contribute to child insets. Complete files use
byte `77`; suspended partials use the previously unused sentinel `0xF3`.
The related CSS rule cache uses version `18`; version `17` already occurs in
local branch history.

### Version 75

Version 75 keeps the serialized layout unchanged but excludes EPUB elements with
the HTML `hidden` attribute. Complete files use byte `75`; suspended partials use
`0xF4`. Both older full and partial layouts rebuild automatically.
Versions 67–74 and partial sentinel 0xF5 already occur in other local branch
history; using fresh identifiers avoids accepting those experimental caches.

### Version 66

Version 66 keeps the version 63 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout. Complete files use
version byte `66`, and suspended partials use sentinel byte `0xF6`.

The stable v1.5.1 release retains these identifiers from RC6. Do not normalize
published RC versions to the previous stable version plus one: v1.5.0 used
`60` / `0xF9`, and RC4 already shipped `61` / `0xF8` with older layout output.
Reusing those identifiers could accept stale RC caches as current. Version 9
per-book reader settings and their version 7/8 migrations remain
readable. Version 10 adds a field-override mask so a book can inherit unrelated
global reader settings.

### Version 62

Version 62 stores one compact source-whitespace bit per word in serialized text
blocks. Touch reader previews use it to reflow words with the selected font
without inferring spaces from device-specific pixel advances. Full and
suspended section caches rebuild together; complete files use version byte
`62`, and suspended partials use sentinel byte `0xF8`.

### Version 61

Version 61 was an earlier v1.5.1 release-candidate cache update. It stores `protectedImageUnits`
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
Focus Reading split-run offsets with the renderer's combined advance and
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
overflow into the reserved status-bar area. Version 49 stores Focus Reading
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
  embedded CSS, image rendering mode, Focus Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- visible-text-offset LUT used to resolve page positions across reflow and sync
- optional per-word Focus Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Focus Reading and Guide Dots as per-word metadata instead of temporary layout words
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

#define EXPECTED_VERSION 86
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
    u8 fontSize;
    u16 lineHeight;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
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
    u32 magic; // 0x535843FF (bytes: FF, "CXS")
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
    u32 hyphenationPatternIdentity;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    bool guideReadingEnabled;
    u8 wordSpacing;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 protectedImageUnits;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;
    u32 visibleTextLutOffset;

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

## Optimizer image transport (PXC2 and COIX)

Optimizer images are optional EPUB sidecars. Original JPEG/PNG assets remain the
compatibility fallback. `META-INF/crossink/optimizer-v1.json`,
`META-INF/crossink/optimizer-images-v1.idx`, and all
`META-INF/crossink/pxc/*.pxc2` entries must use ZIP STORE. PXC2 has its own block
compression; the firmware rejects ZIP-deflated PXC2 before starting an inflater.
The manifest keeps `format: "crossink-optimizer"` and `version: 1`. Each image has
`href`, `pxc`, `width`, `height`, `pxcFormat: "pxc2"`, `pxcBytes` (complete transport
size), and `pixelCrc32`. Missing `pxcFormat` means legacy raw PXC1.

All integers below are unsigned little endian. Fields are serialized explicitly,
not by writing native C++ structures. CRCs use standard IEEE CRC32 (zlib).

### PXC2

The 32-byte header is:

| Offset | Bytes | Field                                                              |
| ------ | ----- | ------------------------------------------------------------------ |
| 0      | 4     | `PXC2`                                                             |
| 4      | 1     | Version 2                                                          |
| 5      | 1     | Pixel format 1: four 2-bit pixels per byte, most significant first |
| 6      | 2     | Header bytes: 32                                                   |
| 8      | 2     | Width                                                              |
| 10     | 2     | Height                                                             |
| 12     | 2     | Row bytes: `(width + 3) / 4`                                       |
| 14     | 2     | Block bytes: 2048                                                  |
| 16     | 2     | Block count: ceiling of raw bytes / 2048                           |
| 18     | 2     | Flags: zero                                                        |
| 20     | 4     | Raw pixel bytes                                                    |
| 24     | 4     | Raw pixel CRC32                                                    |
| 28     | 4     | Complete PXC2 file bytes                                           |

Dimensions are 1–1024, raw data is at most 128 KiB, and there are at most 64
blocks. Each block starts with a 12-byte header: codec (`u8`, 0 RAW, 1 raw DEFLATE,
2 PackBits), zero flags (`u8`), raw length (`u16`), encoded length (`u16`), sequence
(`u16`, starting at zero), decoded-block CRC32 (`u32`). Lengths are 1–2048. All
non-final blocks have 2048 raw bytes. Compressed blocks must be smaller than raw;
otherwise producers use RAW. Every block is independent. PackBits controls 0–127
copy the next control+1 bytes; 129–255 repeat the next byte 257-control times;
128 is rejected. Raw DEFLATE must terminate exactly, with no extra input/output.
Python uses zlib level 8 and `wbits=-15`; the browser uses PackBits/RAW.

The reader reuses a fallibly allocated session workspace (5928 bytes on the
64-bit host; firmware size is compile-time limited below 6500), verifies all
lengths/CRCs and resizes rows into `img_*.pxc.optimizer.tmp`. It checks size and
syncs/closes before publishing. On filesystems that cannot rename over an existing
file, `.optimizer.previous` retains the previous cache until publication succeeds;
failed rollback leaves that backup available for recovery on the next attempt. PXC2 never needs a full raw source temporary file.
The local raw PXC layout remains two `u16` dimensions plus packed rows. No section
cache version change is required. Hardware heap and visual results remain device
acceptance checks, not implied by host workspace accounting.

### COIX version 1

The local index is `/.crosspoint/epub_<hash>/optimizer-images.idx`. Its header is:

| Offset | Bytes | Field                            |
| ------ | ----- | -------------------------------- |
| 0      | 4     | `COIX`                           |
| 4      | 2     | Version 1                        |
| 6      | 2     | Header bytes: 32                 |
| 8      | 2     | Record bytes: 208                |
| 10     | 2     | Record count: 0–256              |
| 12     | 4     | Flags/reserved: zero             |
| 16     | 4     | Manifest central-directory CRC32 |
| 20     | 4     | Manifest uncompressed bytes      |
| 24     | 4     | CRC32 of all records             |
| 28     | 4     | CRC32 of header bytes 0–27       |

Each 208-byte record contains NUL-terminated `href[129]` (offset 0),
`pxcHref[65]` (129), width `u16` (194), height `u16` (196), format `u8` (198,
1 legacy or 2 PXC2), zero flags `u8` (199), complete sidecar bytes `u32` (200),
and pixel CRC32 `u32` (204). Paths are relative, bounded UTF-8; traversal,
backslashes, control characters, colons and percent escapes are rejected.
Sidecars must be beneath `META-INF/crossink/pxc/`. Duplicate image hrefs are invalid;
producers keep one optional sidecar per href and omit unsupported/over-limit
entries. Firmware can resize that sidecar for other layouts.

Reader setup validates a packaged index and copies it through a temporary local
file, or builds it from a legacy manifest while the framebuffer is loaned. Generic
`Epub::load()` does not build indexes. A lookup scans records on SD and retains
only one last hit. Manifest identity changes invalidate the local index. Per-page
preflight reuses local pixels first, then materializes just that page's sidecars,
then extracts original assets as fallback. Legacy ZIP inflation also runs under
the framebuffer loan. Sleep-page generation uses the same preflight. Temporary
index files are cleaned during setup and per-image temporaries during preflight.

Hardware acceptance: clear only the test book's cache, open legacy/PXC2 variants
with SD font and AA, visit/revisit image pages and sleep, compare portrait and
landscape output, and record internal free/largest heap blocks and low-water
marks. Repeat corrupt sidecars, full/read-only SD, interrupted writes and book
replacement at the same path on X3/X4, Sticky (SPI SD) and X4 Pro (SDMMC).

### CSS rules cache revision 18

Revision 18 adds the serialized `list-style-type` property used to number
ordered lists and suppress list markers. It also includes the PSRAM streamed
stylesheet path introduced in revision 16, which admits sources up to 512 KiB
on PSRAM readers while C3 retains its 128 KiB limit. Existing rule-count and
internal-memory guards still apply. Rebuilding an invalid CSS cache also
invalidates section caches through the existing EPUB-load path, so books that
previously cached zero rules can restore hidden content and layout rules.

## S3 scalable reader fonts

Static TTF support uses font-content and backend identities to invalidate
affected EPUB layouts. Section-cache serialization is unchanged. Existing
`.cpfont` files remain supported; see [scalable fonts](scalable-fonts.md) for
limits and lifecycle.

## `/.crosspoint/font-catalog.bin`

### Versions 1–3

Disposable font metadata cache, shared by reader, settings and web font controls.
The 24-byte little-endian header contains magic `0x46434931`, version, a 64-bit
inventory fingerprint, family count (maximum 128), and scalable-font build mode.
It is followed by 152-byte family summaries: a NUL-terminated 128-byte name,
32-bit detail offset/byte count/FNV-1a hash, 16-bit file count, minimum/maximum
point sizes, a scalable flag, three reserved zero bytes, and a 32-bit FNV-1a
checksum of the preceding summary bytes. Detail blocks follow the summaries.
Each detail is three bytes (point size, style, path length) followed by the
UTF-8 path bytes. Paths are at most 255 bytes; families contain at most 256
files. The whole cache is capped at 2 MiB.

Names/range labels load without retaining file paths. Only a requested family
hydrates its detail vector. Missing, incompatible or malformed summaries rebuild
the catalog by scanning names first and writing one family's paths at a time;
invalid detail blocks invalidate the cache for the next request.
Temporary memory failures preserve the font selection and cache for retry.
Writers finish and sync `font-catalog.tmp` before replacing the cache. An
interrupted replacement is safe because no user data is stored here.

On first font metadata access after boot or explicit invalidation, an inventory
walk checks names, directory kinds and file lengths in both font roots and their
immediate subdirectories. It reads no font contents on an index hit, and is not
repeated when catalog RAM is released and reloaded during the same session. This
detects externally added/removed files and changed lengths after restarting;
same-length content-only edits outside the firmware are not detectable by that
check. Firmware upload/delete/move/download paths invalidate explicitly,
including failed dedicated font uploads; USB Drive invalidates before handing
the card to the host. Manage Fonts performs a full rescan; alternatively remove
this cache to force reinspection after external same-length font changes.

EPUB layout cache versions and identities are unchanged by this catalog.

Version 2 rescans static TrueType-outline `.ttf` and `.otf` files. Version 3
rescans again so OpenType CFF faces rejected by earlier firmware enter the
catalog without requiring a manual font refresh. The binary layout is unchanged.

### Daily reading counters (v2)

Device-local counters live in `/.crosspoint/daily_reading/YYYY-MM.a.bin` and
`YYYY-MM.b.bin`. Each month uses at most two 264-byte snapshots, regardless of
how many days were read (dates support 2000–2099). All integers are little-endian:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | Version 2, ASCII `D`, ASCII `R`, reserved zero |
| 4 | 4 | Month's first day index since 2000-01-01 |
| 8 | 4 | Generation, incremented for each snapshot (wrap-safe comparison) |
| 12 | 248 | 31 pairs of uint32 cumulative seconds and acknowledged seconds |
| 260 | 4 | FNV-1a checksum of bytes 0–259, initial hash 2166136261 |

Records after the month's last day must be zero. Acknowledged seconds must not
exceed cumulative seconds. Saves overwrite the older/invalid slot, sync and close
it, then reopen and validate it. Reads choose the newest valid generation. A torn
write falls back to the other slot. New months first establish an empty generation-0
`b` snapshot before creating `a`; a lone torn `b` is an unfinished initialization
and can be retried. Unknown newer formats and two corrupt copies
are preserved and rejected. Unchanged counters do not cause writes. A complete
write whose sync/close reports failure may be recovered on retry.

Tester v1 files (`NNNNN.bin`, `.bin.bak`, `.bin.tmp`) are converted when their month is first
accessed for reading or sync, using fixed memory. A damaged legacy month blocks
that month only; unrelated months can continue recording. Each v1 file is 13 bytes: version
1; uint32 cumulative seconds at offset 1; uint32 acknowledged seconds at offset 5;
uint32 FNV-1a checksum at offset 9, seeded with `2166136261 XOR day_index` over bytes
0–8. Conversion takes the maximum totals and acknowledgments across valid legacy
copies and existing monthly records. Both monthly slots are written and verified
before deleting any originals. Corrupt legacy siblings are deleted before valid
ones, so interrupted cleanup leaves a recoverable source. Failed conversion is
retried; unknown formats or wholly corrupt legacy days remain untouched. SD read
errors stop the operation rather than selecting older data or deleting sources. Once
converted, normal operation only reads/writes monthly snapshots. Do not downgrade
to firmware that only understands v1 after conversion.

Directory scans close before conversion or uploads. Months are visited in date
order by rescanning filenames, avoiding an unbounded file list and directory
cursor invalidation during migration. Checkpoint work uses one 256-byte month
buffer plus small serialized chunks, with no new heap allocation or retained
history buffer. The sync payload and server API are unchanged.

After successful daily stats sync, the reader removes fully acknowledged months
older than the previous calendar month. Current, previous, and future months stay,
as does any month with unuploaded seconds. Cleanup requires a valid local date,
flushes accepted in-memory time first, and validates every existing snapshot;
corrupt, unknown, or unreadable data is retained. The newest acknowledgments must
cover every total in either snapshot. Older snapshots are deleted first so an
interrupted cleanup retains the newest acknowledged copy for retry. Cleanup errors
are logged and retried on a later successful sync without marking the completed
upload as failed. This only removes local daily-time files: uploaded server history,
book progress, and separate global/book statistics are unchanged. Deleted local
history is not downloaded again, so it cannot be re-uploaded to a different server
or account later. Unsynced offline history has no age-based cutoff.

Only accepted page intervals in the existing EPUB/XTC stats paths contribute.
Idle intervals over the configured threshold are rejected in full; menus,
dictionary, input lock, overlays, and reader exit use the existing timer boundaries.
The existing ten-second session minimum applies. Durations remain whole seconds,
with the existing timer's per-interval millisecond remainder discarded. Each
accepted interval captures a fresh wall clock and the configured fixed UTC offset
at its start/end; midnight splits actual intervals, rather than placing accumulated
active time at session start. Missing/invalid time, changed offsets, or wall time
changes exceeding two seconds leave that interval undated. RTC readings are
uncached and reject oscillator-stop/I2C failures. RTC-less devices use valid system
UTC after NTP; this does not add background NTP or correct deep-sleep clock drift.
No historical duration is inferred from global totals, buckets, or history bits.

RAM is bounded: two day counters plus at most nine seconds of pending short-session
intervals per reader. Saves are debounced to 300 accepted seconds (five minutes),
with periodic checkpoints waiting for one second of quiet reader input/rendering.
Explicit global-stat saves, reader exits and sync still flush immediately. A sudden power loss can
lose uncheckpointed accepted seconds (normally under 300, but longer while paging), plus the current page
interval that has not yet been accepted. Normal sleep commits through reader exit.
Daily counters survive book/global-total resets and book moves/merges; they are
monotonic device history for sync, independent of those resettable totals. Deleting
this directory manually destroys that history; it cannot be reconstructed.

Stats sync extends `PUT /api/v1/stats/global` with an optional `daily` array:
`[{"date":"2026-10-01","seconds":61}]`. Dates are local calendar dates, not UTC
timestamps. Firmware streams one changed day per request in the existing 1536-byte
buffer; it marks that counter uploaded only after `accepted_daily: 1`. Old servers
can still accept aggregate stats, but cannot silently discard daily history and
acknowledge it. Failed requests remain eligible for retry, including recovered
`.tmp`/`.bak` records. Nearby-device snapshots are never uploaded as local history.

## Internal language cache (v1)

The last 128 KiB of the existing `spiffs` data partition contains two 64 KiB
slots. No application partition or partition-table entry changes. All integer
fields are explicitly serialized little-endian; no packed C++ structs are cast
onto flash bytes.

A slot begins with a 256-byte header: a 16-byte `CILANG` v1 ownership marker,
commit word at offset 16, 64-bit generation at 20, total used bytes at 28,
16-bit record count at 32, RTL flag at 34, and CRC32 at 36. Fixed NUL-terminated
UTF-8/ASCII metadata fields are code[32] at 40, name[96] at 72, and keyboard[32]
at 168. Reserved header bytes are zero. CRC32 covers bytes 20 through the end
of used data with the checksum field treated as zero. The commit word is
written last. The marker identifies language-cache contents; permission to use
the region comes from its firmware data-partition role. Applying a language may
replace previous filesystem contents in the target slot. Other partition bytes
and the pinned language slot remain unchanged. Interrupted initial provisioning
can be retried without needing an intact prior ownership marker.

Records follow sequentially: 64-bit FNV-1a key identity, 64-bit FNV-1a English
reference signature, 16-bit string length including NUL, then the UTF-8 string.
Only translated entries known at installation are stored. Firmware matches
stable identities/signatures at startup and builds a uint16_t offset per current
StrId; 0xffff means English fallback. Numeric StrId order is not an on-flash ABI.
Checksums, lengths, UTF-8, metadata, and current-key duplicates are validated
before a mapping is exposed. The mapping remains pinned until restart.

Settings keep the preferred code in the existing `language` JSON key and the
selected generation in `languageCacheGeneration`. A zero generation permits a
legacy preference to find the newest matching valid slot. Normal saves use a
synced `.tmp` and recoverable `.bak`; a remaining backup denotes an unfinished
publication and is restored before settings are loaded or saved.

### SD-backed cache on PSRAM devices

When `spiffs` is missing or fails the language storage size check, PSRAM-equipped
devices use `/.crosspoint/asset-cache.bin` instead. Its fixed size is `0x360000`;
the same language slots occupy its last 128 KiB and the same hyphenation banks
occupy the preceding bytes. This is a new backing store, not a format-version
change. Compatible flash layouts keep their original backend; flash I/O failures
do not silently select SD. No partition-table or application-slot writes occur.

Read-only startup treats an absent image as erased storage. Explicit installation
creates and fills the image with `0xff`. Writes sync and close before returning,
so the existing checksum and commit-last protocols still apply. A truncated
image is rejected rather than overwritten. An incomplete initial creation can
be removed and the packs reinstalled; SD filesystem durability still depends on
the card. The cache holds installed resources, not book progress or settings.

Startup validation and mapping share a scoped read-only SD handle per consumer.
Consecutive chunks read at the current file position without seeking; bank/slot
changes seek only when necessary. The handle closes on every return path before
normal reader or USB activity, and writes are rejected while a read session is
active. No additional PSRAM buffer is needed for this read optimization.

Each selected mapping is copied to a fallible PSRAM allocation and kept immutable
until reboot, without retaining an SD file handle. Language slots are 64 KiB;
the active hyphenation mapping uses only the bank's used bytes, bounded by
`0x1a0000`. There is no internal-RAM allocation fallback. Missing/corrupt data or
allocation failure leaves the built-in language/patterns available.

## SD hyphenation packs and flash banks

SD path: `/.crosspoint/hyphenation/hyph-<code>.cphyph`. The upstream CPHY v1
header is 24 bytes, with all multibyte integers little-endian:

- 0: magic `CPHY` (4 bytes)
- 4: version `1` (u8)
- 5: primary language code (2 lowercase ASCII bytes)
- 7, 8: minimum prefix/suffix characters (u8 each; currently 2/2)
- 9: flags (u8; currently zero)
- 10: reserved (u16; zero)
- 12: root offset into payload (u32)
- 16: payload size (u32)
- 20: payload CRC32 (u32, IEEE/zlib)

The payload is the existing Hypher trie with its original four-byte root prefix
removed, unchanged from the firmware table. The root offset excludes that prefix.
The header language must match the filename chosen by the manager.

CrossInk bank format is independent of upstream's internal flash bank format.
Two 64 KiB-aligned banks occupy the partition before the UI-language slots.
For the current 0x360000-byte partition they begin at 0 and 0x1a0000 and each
hold 0x1a0000 bytes. Only the inactive bank is erased/written; language slots at
0x340000 and 0x350000 are never touched.

Each bank starts with a 256-byte header. Bytes 0-7 are `CIHP`, version byte 1,
and three zero bytes. Little-endian u32 fields at offsets 8, 12, 16 and 20 hold
generation, entry count, used size and header CRC32. Offset 24 is the commit
marker `0x50485950`, written last. Offset 28 is reserved zero. The header CRC
covers bytes 0-23 (checksum field treated as zero) and 28-255; it excludes the
commit marker. Unused header bytes are zero.

Up to ten 20-byte entries begin at offset 32: language code (2 bytes), prefix
and suffix (one byte each), bank-relative payload offset, payload size, root
offset and payload CRC32 (four u32 values). Payloads start at offset 4096 and
are packed in entry order with four-byte alignment. Duplicate/unsupported
languages, invalid bounds, uncommitted banks and bad header/payload checksums
are rejected. Boot selects the highest fully valid generation. Generations do
not wrap; exhausting u32 rejects a further update. A successful update requires
a restart before another operation or activation of its new mapping.

## OPDS server download folders

`/.crosspoint/opds.json` stores an optional `downloadFolder` string on each entry in `servers`. Missing or empty means inherit the global `opdsDownloadFolder` setting; `/` explicitly selects SD root. Other values are normalized absolute SD folder paths (at most 63 UTF-8 bytes). Existing files need no migration. No EPUB cache or settings binary format change is required.

### KOReader server configuration v3

`/.crosspoint/koreader.json` adds `serverType` (0: CrossPoint, 1: KOSync, 2: Other) and up to two `headers` entries with `name` and device-obfuscated `value_obf`. Version 2 configurations retain their URL and opt-ins; the hosted default maps to CrossPoint and custom URLs to KOSync. The pre-v2 legacy-server migration remains separate. No EPUB cache layout changes.

### EPUB explicit-dash wrapping

Section cache version 96 (partial version `0xD1`) adds breakable, space-free token boundaries after visible hyphens and dashes. Both complete and suspended caches from earlier versions rebuild automatically.

Section cache version 97 (partial version `0xD2`) prevents justification from stretching boundaries inside hyphenated words. Complete and suspended layouts from earlier versions rebuild automatically; the binary payload is unchanged.


## Reading session outbox

New measured sessions supplement the existing book/global counters and daily
history; no existing stats file or EPUB cache format changes. Each tracking
session keeps a random 128-bit ID and cumulative active seconds, checkpointed
after another 300 accepted reading seconds (five minutes) and on normal reader
exit or tracking disable. Periodic writes wait until reader input/rendering have been quiet for
one second; explicit saves bypass that delay. Matching previews of the existing book/global counters are saved without
consuming the live session, including temporary overlays and restart handoffs. A checkpoint excludes idle page intervals using the existing timer.
Intervals with an unavailable/corrected clock remain in the existing counters
but do not produce fabricated dated sessions. The outbox is independent of book
cache cleanup and follows the configured KOReader document identity at capture.

`/.crosspoint/reading_sessions/<32-lowercase-hex-id>.bin` uses SyncFiles atomic
writes, CRC validation, and `.tmp`/`.bak` recovery. Fields are written separately
in the firmware's little-endian integer representation, without struct padding:

| Field | Bytes |
|---|---:|
| Format version (1) | 1 |
| Session ID, ASCII hex without terminator | 32 |
| Document ID, ASCII hex without terminator | 32 |
| UTC start timestamp, seconds | 8 |
| UTC end timestamp, seconds | 8 |
| Cumulative active reading seconds | 4 |
| Signed UTC offset, minutes | 2 |
| CRC32 over preceding bytes | 4 |

Session IDs remain unchanged as checkpoints grow and across upload retries.
Uploads use the existing stats credentials and privacy switches, with at most
20 records per network pass. A specific `accepted_session_ids` acknowledgment is
required before deleting the local file and its recovery copies. An older server
without `/api/v1/stats/sessions` leaves the outbox intact and does not invalidate
support for the older stats endpoints. Corrupt/unreadable records and failed
uploads are retained. Reading fragments under ten seconds are not exported,
matching the current BookOrbit minimum; existing counters keep their own rules.

This adds future session detail only. Old totals, daily time, and streak history
are retained without guessing historical sessions. Clock/offset changes and gaps between accepted active intervals start a
new session identity, so a paused reader does not spread reading time across
unread hours or dates. Future-dated records are retained after clock correction
while other valid sessions continue to upload. Device display continues to use the existing counters;
imported aggregate stats never enter this local measured-session outbox.

Automatic BookOrbit export requires a resolved book match. EPUB progress normally
supplies the matching metadata. XTC's stats-only protocol currently supplies no
title/author, so its sessions require an existing or manually established match
to the corresponding EPUB in BookOrbit.

## Protected EPUB `<book>.key`

Version 2 stores `FIBK` (4 bytes), version `2` (1 byte), content-key length
(1 byte: 16 or 32), expiry (8-byte little-endian UTC epoch seconds; zero means
no expiry), random IV (12 bytes), wrapped content key (16 or 32 bytes), and
AES-GCM tag (16 bytes). The complete 14-byte header is authenticated as additional
data. The wrapping key is SHA-256 of `book-key:` followed by the 32-byte internal
NVS device secret. The file is 58 or 74 bytes; unexpected lengths are rejected.
Version 1 remains readable and has a 13-byte header without the key-length byte,
with a fixed 16-byte content key.

Writes use `.tmp` staging and `.old` recovery. The device secret is not stored on
the SD card. Existing EPUB cache formats are unchanged; those caches may contain
decoded content and are usable by the reader only after its book-open access check.
