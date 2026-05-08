# Changelog

## [v1.2.10]

### Added
- Add Lyra Carousel theme
- Add a `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid
- Add real EPUB `<hr>` rendering so horizontal rules now display as visible separators instead of being ignored
- Add a per-session auto page turn interval picker so EPUB readers can choose any value from 5 to 120 seconds instead of only the old preset list
- Add abilty to render block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation in reader fonts (PR #104)

### Fixed
- Keep EPUB list bullets attached to the first paragraph in `<li><p>...</p></li>` list items
- Keep EPUB/XTC thumbnail cache paths, Recent Books cover updates, and carousel snapshot reads consistent when thumbnail dimensions or cache files change
- Serialize `GfxRenderer` bitmap scratch-buffer access so concurrent image draws cannot reuse the same temporary rows at the same time
- Harden JPEG image scaling, EPUB thumbnail caching, and large CSS rule handling against crashes, stale cache files, and long-session allocation failures
- Serialize SD-card and display access on the shared SPI bus to prevent task-ownership crashes during state saves, sleep transitions, and other concurrent render/storage activity
- Guard SPI bus lock acquisition so a failed recursive mutex take no longer marks the lock as held and triggers a mismatched release
- Harden EPUB section-cache writes and promotion so truncated SD writes fail fast, temp caches are synced before rename, and invalid page-cache files are less likely to persist across reloads
- Reject invalid serialized string lengths before allocation so corrupted cache data cannot trigger oversized string resizes during reads
- Relax the EPUB low-memory image fallback so inline images are no longer suppressed solely because the heap is fragmented while overall free memory is still above the safety threshold
- Reduce CSS-parser heap fragmentation during EPUB indexing so image-heavy chapters are less likely to crash while pre-indexing the next section
- Avoid an EPUB CSS-cache rebuild crash after clearing a book cache by growing the CSS rule table in guarded chunks instead of reserving the full rule limit at once
- Keep EPUB CSS selector lookups valid while loading stylesheets by maintaining sorted rule storage as rules are inserted
- Avoid rebuilding the current EPUB section when changes to Reader Options affect only render-quality settings.
- Skip image decoding during the font prewarm scan.
- Clear cached EPUB metadata for books inside deleted folders so stale `/.crosspoint/epub_*` directories are not left behind

## [v1.2.9.1] - 2026-05-03

### Changed
- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed
- Fix power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions. Those that had short-press power button to act as sleep saw unstable behavior previously. This should be fixed now
- Fix a potential crash when using `Go to %` in EPUBs
- Fix a potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid
