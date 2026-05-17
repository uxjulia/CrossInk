# Changelog

## [Unreleased]

### Added
- Added a Common Issues guide with user-facing fixes for WiFi, uploads, EPUB cache rebuilds, covers, reading stats, and OTA update problems.
- Added an in-reader confirmation message when a shortcut turns tilt-to-turn on or off.
- Added a 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.

### Fixed
- Disabled WiFi power saving and buffered SD writes during regular file downloads so OPDS book downloads and SD font downloads can run faster and more consistently.
- Reduced stack usage when copying files over WebDAV.
- Fixed Lyra Carousel selected menu icons rendering as black squares on X3 devices.
- Relaxed KOReader Sync auth response validation so compatible self-hosted servers that return valid JSON on successful login can authenticate.
- Fixed OPDS browsing on memory-constrained builds so large catalog feeds fail safely instead of rebooting the device.
- Fixed a crash when opening the XTC chapter selector on memory-constrained builds.
- Kept the Minimal theme home navigation button labels in English so all four labels stay consistent and compact.

### Changed
- Kept OPDS feed parsing safer on low-memory devices while preserving older parser iteration helpers for future OPDS work.
- Reduced unnecessary OPDS browser list clearing when moving between catalog feeds.

## [v1.2.11.1] - 2026-05-15

### Changed
- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed
- Included Lyra Carousel by activating the build flag `DCROSSINK_ENABLE_LYRA_CAROUSEL=1`
---
## [v1.2.11] - 2026-05-14

### Added
- Added new personal theme: "Minimal"
- Added a custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- Added an in-reader Controls shortcut so you can customize your buttons without leaving the book.
- Added bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Added a confirmation message after deleting a book's cache from the reader or File Browser.
- Added a File Browser long-press action for deleting an EPUB or XTC book's cache
- Added a File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed
- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.

### Fixed
- Fixed inline EPUB images disappearing in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- Fixed the SD-card font picker reopening immediately after selecting a font from Settings > Reader > Font Family.
- Fixed in-reader font-size changes for SD card fonts not working
- Fixed in-reader SD-card font changes not always rebuilding the current EPUB page layout.
---
## [v1.2.10] - 2026-05-11

### Added
- Added a `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- Added more flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Added a per-session auto page turn interval picker with values from 5 to 120 seconds.
- Added a file-browser Home/Back long-press action for toggling hidden files and folders.
- Added EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Added reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Added simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed
- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed
- Fixed reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- Fixed RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Fixed font and glyph handling by reducing persistent SD-card font advance-cache memory, releasing optional font caches before image extraction only when heap is tight, and showing a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- Fixed KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- Fixed EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- Fixed EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- Fixed EPUB low-memory and cache safety by skipping optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, failing safely with a malformed-book warning and Home exit path, rebuilding incompatible fork-written caches, and handling low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Fixed a Home crash after clearing reading cache by skipping optional EPUB thumbnail rebuilds when the source EPUB cache is missing.
- Fixed reader prewarm behavior by skipping image decoding, keeping mixed-style font glyphs cached together, and avoiding section rebuilds for render-quality-only option changes.
- Fixed concurrent render/storage crashes by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Fixed Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling so cached book data stays in sync and grid covers fill their slots correctly.
- Fixed simulator build configuration so SDL2 and simulator-provided network/OTA shims compile cleanly.
---
## [v1.2.9.1] - 2026-05-03

### Changed
- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed
- Fix power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions. Those that had short-press power button to act as sleep saw unstable behavior previously. This should be fixed now
- Fix a potential crash when using `Go to %` in EPUBs
- Fix a potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid
