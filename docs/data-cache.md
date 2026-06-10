---
title: Data Cache
nav_order: 16
---

# Data Cache

CrossInk caches data aggressively on the SD card to minimize RAM use. The ESP32-C3 has about 380 KB of usable RAM, so rebuilding every book structure in memory on every open would be too expensive.

The main data directory is `.crosspoint` on the SD card. It stores render caches and persistent user/device data.

## Directory Layout

```text
.crosspoint/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── synced_stats/           # Stats snapshots received from other readers
├── settings.bin            # Device settings
├── state.bin               # Last-opened book and sleep/session state
├── recent.bin              # Recent books list
├── bookmarks/              # Bookmark files, one per book
├── home_carousel_cache.bin # Lyra Carousel home-screen snapshot cache
├── epub_12471232/          # Each EPUB is cached to epub_<hash>
│   ├── progress.bin        # Reading position (chapter, page, etc.)
│   ├── stats.bin           # Per-book reading stats
│   ├── cover.bmp           # Book cover image, once generated
│   ├── thumb_*.bmp         # Home/recent-books thumbnail images
│   ├── book.bin            # Book metadata, spine, table of contents, etc.
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

## Clearing Cache Data

Deleting the entire `.crosspoint` directory resets caches, settings, saved network/server data, bookmarks, recent books, reading progress, and reading stats.

To clear EPUB/XTC render caches from the device UI without deleting settings or global stats, use:

**Settings > System > Clear Reading Cache**

## Book Moves And Cache Identity

Cache folders are path-based. Moving a book file can create a new cache directory, so the moved copy may start with fresh reading progress unless the firmware has a migration path for that move.

Cache data is not automatically removed when a book is deleted.

For binary file layout details, see [File Formats](./file-formats.md).
