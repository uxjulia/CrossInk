---
title: Data Cache
nav_order: 16
---

# Data Cache

A cache is a saved copy of work the reader has already done, so it does not have to repeat it. When you open a book for the first time, CrossInk works out where every page break goes and saves the result on the SD card. The next time, the book opens much faster. The Xteink X3 and X4 have only about 380 KB of working memory, so saving this work to the SD card is much cheaper than redoing it each time.

Everything is kept in a hidden folder called `.crosspoint` on the SD card. It holds the caches and also your personal data: settings, reading progress, bookmarks, clippings, reading stats, and saved Wi-Fi networks. CrossInk does not change the contents of your book files.

## Clearing Cache Data

Deleting the whole `.crosspoint` folder resets everything in it: caches, settings, saved Wi-Fi and server details, bookmarks, recently opened books, reading progress, and reading stats. Only do this if you want a completely fresh start.

If a book looks wrong after an update, or you want to free up space, you can clear just the book caches without touching your settings or all-time stats:

**Settings > System > Files & Cache > Clear Reading Cache**

Books are laid out again the next time you open them, so the first open is slower.

## Book Moves And Cache Identity

Each book's cache is tied to the book's location on the SD card. If you move or rename a book on a computer, CrossInk sees it as a new book and it may start with fresh reading progress. Moving a book inside CrossInk (for example, to the Read folder or with the File Browser move actions) keeps its cache and bookmarks.

Reader settings such as font, layout, and reading aids normally come from the global Reader settings. If you change a setting while reading a book, only that setting is saved for that book. Every other setting still follows your global choices. A book's render mode is also saved per book, so a troublesome book can be switched to Balanced or Light rendering from the File Browser or Library long-press menu before you open it.

Clippings (highlights) are saved separately from the book cache, in `/.crosspoint/clippings/`, so clearing the reading cache does not remove them. CrossInk also adds each clipping to a readable text file, `/My Clippings.txt`, in the top folder of the SD card. That file only ever grows. If you delete a clipping on the reader, it is removed from the reader but stays in `/My Clippings.txt` until you edit that file yourself.

Deleting a book inside CrossInk also removes its cache. If you delete books from a computer, their old cache folders stay behind until you run **Clear Reading Cache**.

Backups of your all-time reading stats are kept outside `.crosspoint`, in:

```text
/.crossink-stats-backup/
```

## Library

The Library screen lists the books on your SD card. See the [User Guide](./user-guide.md#34-library-screen) for how to search, sort, and use it. This section covers what it saves.

To make the Library fast, CrossInk keeps an index of your books (titles, authors, file locations, and sort order) in `.crosspoint`. It checks the index against the SD card on the first visit after the reader starts, after you change files in the File Browser, and after a download or file transfer. Other visits reuse the saved index. If you changed files from a computer while the reader stayed on or asleep, choose **Refresh Library** to scan the card again.

- Book details are saved as each book is read, so a scan that is cancelled, fails, or loses power picks up where it stopped. Deleting `library.meta` and `library.metd` is safe. They are rebuilt as books are read.
- **Library Settings > Use Book Metadata** chooses between the title and author inside each EPUB and the filename. TXT, Markdown, and XTC files always use the filename.
- **Date Added** uses the time the file arrived on the card. Some copy tools keep the original file's date, which can change the order.
- **Recently Opened** shows only books in your reading history (up to 18), newest first. Marking a book unfinished does not add it to the history. Opening it does.

## Advanced: Folder Layout

This is for people who browse the SD card on a computer. You do not need it for normal use.

```text
.crossink/
├── languages/              # Editable UI translations
│   └── fonts/              # UI fallback font families
└── hyphenation/            # Installable EPUB hyphenation packs

.crosspoint/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── daily_reading/          # Device-local daily reading seconds and sync acknowledgments
├── synced_stats/           # Stats snapshots received from other readers
├── asset-cache.bin         # Language/hyphenation cache on PSRAM devices with small factory flash
├── crossink-settings.json  # CrossInk device settings
├── settings.json           # Legacy settings fallback, if present
├── settings.bin.bak        # Legacy binary settings file after migration, if present
├── state.json              # Last-opened book and sleep/session state
├── state.bin.bak           # Legacy binary state file after migration, if present
├── recent.json             # Reading history for Home and Library
├── library.idx             # Library titles, authors, paths and sort indexes
├── library.meta            # Library EPUB metadata cache: slot table
├── library.metd            # Library EPUB metadata cache: parsed metadata records
├── recent.bin.bak          # Legacy binary recent-books file after migration, if present
├── wifi.json               # Saved Wi-Fi networks
├── opds.json               # Saved OPDS servers
├── koreader.json           # KOReader sync credentials
├── sync_pending.bin        # Books changed since their last sync upload (with .bak)
├── sync_state.bin          # Sync catch-ups, prepared folders and feed cursors for the current server account (with .bak)
├── sync_ids.bin            # Document id -> book path for books being read, used by the progress pull
├── bookmarks/              # Bookmark files, one per book
├── clippings/              # EPUB clipping/highlight files, one per book
├── home_carousel_cache_<index>.bin # Lyra Carousel artwork cache for each book position
├── sleep_frame.bin         # Temporary sleep overlay framebuffer, when used
├── epub_12471232/          # Each EPUB is cached to epub_<hash>
│   ├── progress.bin        # Reading position (chapter, page, etc.)
│   ├── stats.bin           # Legacy per-book reading stats
│   ├── stats_v6.bin        # Version 6 per-book reading stats
│   ├── reading_stats_off   # Present when stats tracking is disabled for this book
│   ├── reader_settings.bin # Per-book reader settings, render mode, and auto-page-turn interval
│   ├── sync.bin            # Cached sync document id and catch-up stamps (with .bak)
│   ├── cover.bmp           # Book cover image, once generated
│   ├── cover_absolute.bmp  # Four-tone image-mode cover (generated separately)
│   ├── cover_crop_absolute.bmp # Cropped four-tone image-mode cover
│   ├── thumb_*.bmp         # Home screen thumbnail images
│   ├── book.bin            # Book metadata, spine, table of contents, etc.
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

The `_absolute.bmp` cover files use evenly spaced gray levels. For unfiltered book-cover sleep screens, the Picco SSD2677 generates `cover_picco_image.bmp` (Fit) or `cover_crop_picco_image.bmp` (Crop) from the source artwork on first use. The separate names allow existing caches to upgrade automatically.

For binary file layout details, see [File Formats](./file-formats.md).
