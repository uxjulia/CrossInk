# Changelog

## [Unreleased]

### Added
- Added a Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Added a Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- Added aggregate all-time Reading Stats support from peer-synced per-device stats files.
- Added an in-reader confirmation message when a shortcut turns tilt-to-turn on or off.
- Added a 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.

### Fixed
- Fixed the in-reader Customise Status Bar screen in landscape so the list no longer extends under the button labels.
- Fixed manual WiFi connections from Settings returning immediately to the settings list after a saved-password or open-network connection succeeded, so the connected status and IP address are shown first.
- Fixed missing Vietnamese labels for the sleep timeout resume settings.
- Fixed File Browser and Lyra Carousel icon alignment issues in icon-based themes.
- Reduced grid-like and over-zoomed artifacts on Lyra Carousel and Minimal theme's EPUB cover thumbnails by cropping normal covers before dithering while containing unusual cover ratios.
- Reduced duplicate Home progress/stat loading when returning from another screen.
- Fixed EPUB cache folder keys so they use a stable path hash across firmware builds, migrate older cache folders when possible, and rebuild stale section caches for the latest low-memory layout fixes.
- Improved low-memory EPUB handling by laying out very long text blocks earlier, streaming table fallback content when heap is tight, failing safely on large OPDS feeds instead of rebooting, and clarifying the warning text.
- Reduced sleep-entry memory and battery risk by reusing cached sleep-screen assets, idling OPDS pages normally after load, and putting the X3 tilt sensor back to sleep outside the reader.
- Improved network and SD-card font download reliability by disabling WiFi power saving during transfers, reducing WebDAV stack usage, tolerating longer stalls, retrying interrupted font files, and freeing active reader fonts when needed.
- Fixed a crash when opening the XTC chapter selector on memory-constrained builds.
- Fixed the Font Size setting to follow the actual sizes installed for the selected SD-card font family.
- Relaxed KOReader Sync auth response validation so compatible self-hosted servers that return valid JSON on successful login can authenticate.

### Changed
- Moved the full-time page-as-sleep behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Moved the in-reader Footnotes shortcut above Select Chapter when footnotes are available on the current page.
- Reduced unnecessary screen refresh and list-clearing work during OPDS browsing and SD font downloads so transfers spend more time downloading and less time repainting progress.

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
- Added a downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- Added a File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed
- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed
- Fixed inline EPUB images disappearing in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- Fixed the SD-card font picker reopening immediately after selecting a font from Settings > Reader > Font Family.
- Fixed in-reader font-size changes for SD card fonts not working
- Fixed in-reader SD-card font changes not always rebuilding the current EPUB page layout.

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
