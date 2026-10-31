## [Unreleased]

### Added

- Discover and upload static `.otf` fonts on ESP32-S3 readers, including OpenType CFF fonts and CFF files named `.ttf`, from SD or the Fonts web upload. Variable fonts and font collections remain unsupported (CrossPoint #3646, #3717).
- Assign Select Chapter to shortcuts, Quick Actions, or touch gestures to open the chapter list directly in EPUB and XTC books.
- Track active reading seconds by local day, sync daily reading time to CrossPoint Sync, and show today's total and 7-day average on the This Device reading stats page.
- Preview OPDS book descriptions before downloading, with scrollable text on touch and button devices. Long descriptions show a bounded excerpt.
- Sync a selected EPUB’s progress or an XTC book’s reading stats from its Library or File Browser context menu; Sync Folder and Sync Books include XTC stats too.
- Use server filenames for OPDS downloads to match reading progress across devices; missing or unsafe names fall back to Author - Title.
- View device hardware, firmware/build identity, storage, and memory diagnostics in Settings → System → About without exposing network or device identifiers. Export allowlisted diagnostics and global preferences as local support JSON after confirming the scope; optional EPUB overrides are anonymous, and network/device identifiers, credentials, and reading history are excluded.
- Turn image grayscale on or off globally or for individual EPUB books, independently of text anti-aliasing.
- Use Home/Reader shortcuts on touch devices.
- Customize reader and global status bars: choose Small, Medium, or Large text for each reader bar, set a separate global text size, choose battery display (Icon + %, Icon Only, or % Only) independently for the top, bottom, and global bars, and hide either reader bar without clearing its assigned slots.
- Finished EPUB and XTC books show a checkmarked book icon in the File Browser and Library list.
- Calibrate each screen edge in Settings → Display → Screen Calibration to keep text and status bars clear of the bezel. Calibration stays on the device when changing SD cards.
- Choose Directional or Legacy menu navigation in Controls to restore the previous button behavior in Settings and the reader menu.
- Sync Books from the new Sync Server section in File Transfer to sync progress, reading stats, and clippings for every book on the SD card in one pass, without needing a Library refresh. Sync Reading Stats and Sync Clippings, shown there when that data is turned on under What to Sync, upload only stats or only clippings without touching reading positions; after a one-time catch-up they send only books changed since the last sync.
- Render publisher drop caps, bordered and shaded blocks, preserved whitespace, and more CSS selectors in EPUBs; scalable fonts also support mixed sizes within a line (CrossPoint #3891).
- Install and edit interface languages from SD files, including community languages, with English fallback for missing translations. Browser translation editors and release downloads use an automatically generated, versioned English template and compatibility catalog.
- Choose a UI Font on ESP32-S3 devices to display missing characters in book titles and filenames while retaining the built-in font for supported text.
- Install, update, and remove EPUB hyphenation packs from SD; English remains built in, and book layouts refresh when their packs change.
- Show the current chapter title under the book title in the reader menu on button devices and in the X4 Pro pull-down drawer; the drawer tightens its spacing to keep the handle on screen in landscape.
- Tighten or widen Word Spacing with a -4 to +4 slider and live preview.
- Adjust Character Spacing from -5 to +5 with a live preview in Reader settings and the in-book Reader Font menu.
- On touch devices, adjust EPUB text selection with draggable handles and use the floating Look Up and Clip toolbar. Clipping remains available without an installed dictionary.
- Adds batched folder syncing for book progress, reading stats, and clippings when servers support it
- Render EPUB headings and CSS font sizes on X3/X4 using available bitmap sizes; optional SD sizes stay within a small memory budget and fall back safely when memory is tight.
- Choose whether the built-in EPUB upload optimizer removes embedded fonts; removal is on by default and can be remembered in the browser.
- Render EPUB fieldsets as outlined boxes with padding, honoring publisher border styles and continuing across pages.
- Show a one-time What's New screen after updating, with a one-click Update for an outdated language pack; reopen it anytime in Settings → System → Release Notes.
- Choose a download folder for each OPDS server on the device or web portal, with options to use the shared default or SD root.
- Choose a sync server type and custom authentication headers; compatible servers can receive ISBN, ASIN, and series metadata when sharing metadata is enabled.
- Swipe left on File Browser, Library list, and bookmark rows to reveal a delete button, with confirmation before removal (adapted from Uri Tauber, CrossPoint #3712).
- Fill missing characters throughout the UI, including translated menu labels and dialogs, using the selected UI Font while preserving built-in characters and the reading font.
- Assign Rotate Page 180° to Power, Home, front-button long presses, and button chords as well as side buttons, from device or web settings.
- Assign separate front Left and Right long-press shortcuts, with the same actions as side buttons. Existing shared assignments migrate automatically.
- Pause and resume page-turn taps and swipes with the Disable Page Turn Gestures shortcut, without changing menu access or other touch gestures.
- Enable Tilt: Next Page Only in Controls to advance a page with either tilt direction on supported devices.
- Jump to a percentage in XTC and XTCH books from the reader menu, using the same slider and numeric keypad as EPUB books.
- Install SD card plugins with browser interfaces, on-device catalogs, sign-in, and configurable download folders. Enable Plugins in Settings → System, then open Plugins from Home. Download the optional Plugin Hub to discover, install, and update compatible plugins.
- Center Status Bar option for the top and bottom reader status bars, which centers all items in one row joined by a •, ·, –, or | separator.
- Browse OPDS 2 JSON catalogs, groups, facets, publication descriptions, and indirect EPUB links using the same bounded catalog storage as Atom feeds. OAuth and LCP service fulfillment are not included (CrossPoint #3629).
- Render non-interlaced GIF images, covers, and thumbnails in EPUBs, using the first frame of animated files (CrossPoint #2299).
- Uninstall an installed plugin after holding Confirm for one second or long-pressing its row, with confirmation before its files are removed (CrossPoint #4100).
- Choose Cover Overlay to place a transparent sleep image over the current book cover (CrossPoint #3770).
- Sync combined reading streaks back from CrossPoint Sync, including reading on other devices, while keeping local reading and upload totals separate.

### Fixed

- Keep font upload and delete feedback visible while scrolling on the web portal, including mobile, and prevent older notification timers from hiding newer messages (adapted from CrossPoint #3984).
- Keep web dialogs open when a text-selection drag ends outside the dialog, while preserving backdrop clicks and upload-in-progress protection (adapted from Crestwave, CrossPoint #3931).
- Normalize new web-upload filenames to composed Unicode (NFC), including duplicate checks, so accented filenames remain accessible across platforms (adapted from serialx, CrossPoint #3961).
- Explain OPDS feed failures caused by credentials, withheld credentials across origins, server errors, or no response (adapted from Marco Rojas, CrossPoint #3872).
- Sync accurate KOReader positions for bare body text and nested div/span content, skip whitespace-only anchors, and keep chapter-end progress in real text (CrossPoint #3923).
- Complete missing translations in all 27 community languages, use concise UI labels, and support complete built-in language tables without increasing offset-table size.
- Reduce SD-font cache allocations while preserving kerning and dictionary ligatures, including failed kerning loads (adapted from serialx, CrossPoint #3831 and #3838).
- Resolve EPUB3 series refinements by collection ID even when interleaved or declared out of order, and refresh stale book/library metadata (series parser adapted from jadehawk, CrossPoint #3804).
- Keep button-keyboard tips steady when text becomes empty or non-empty, and hide tips when they cannot fit above the keys (CrossPoint #3863).
- Preserve initial publisher spacing on long EPUB paragraphs without repeating it during incremental layout (CrossPoint #3875).
- Improve low-memory EPUB layout and image handling: limit temporary memory for long text runs, retry image sizing with a lower-memory fallback, and refresh the reader when background image checks finish (YACP 4bfcfb4).
- Make large and interrupted downloads more reliable: complete large WebDAV files, safely resume or restart interrupted book and font downloads, and reduce memory use for large HTTPS transfers. On X3/X4, free OPDS catalog memory during book downloads and restore the same browsing position when finished or cancelled, keeping slow-download cancellation stable under low memory.
- Hide EPUB section-break rules when the publisher suppresses their borders, avoiding duplicate ornaments.
- Translate reading statistics, stats tracking controls, and the Minimal Stats theme label into Polish.
- Dictionary lookup now recognizes French words with contracted prefixes, such as l’histoire and qu’après.
- Page-turn shortcuts no longer crash at the end of an EPUB or while a chapter is loading.
- Preserve EPUB image colors and gray detail in Dark Mode, including repeated page redraws.
- Hide the Cover Grid theme from web settings on devices without PSRAM, matching the device settings menu.
- Apply KOReader or Nearby progress using each book’s render-mode cache, and return to the same File Browser or Library view after context-menu sync.
- Prevent redraw races during screen setup, Calibre upload naming, and OPDS catalog navigation; settings popups with short notes no longer freeze the screen.
- Prevent a suppressed TXT menu-button release from opening the menu again.
- Capture serial screenshots consistently and preserve pre-crash logs while restarting.
- Keep transfer workflows responsive: USB waits yield to background tasks, and Nearby transfers and firmware downloads avoid full-card free-space scans while incomplete writes still fail safely.
- Remove incomplete File Transfer uploads after SD write failures so they can be retried.
- Preserve the previous file when a WebDAV replacement fails, and report settings-save failures in the web portal.
- Keep daily, manual, and clockless reading-stat backups separately, and prune numbered backups in the correct order after 999.
- KOReader-only sync servers no longer end every sync with "extra upload failed"; stats and clippings show "Requires CrossPoint Sync" instead.
- Show full-width Forget and Cancel buttons after a saved Wi-Fi network fails to connect on button devices, with Cancel selected by default below Forget.
- Keep the Auto Page Turn interval in the bottom reader status bar, preserving book and chapter titles in the top bar.
- Keep side-button hints inside calibrated screen edges in landscape.
- Fix ESP32 firmware builds failing to compile hyphenation pack installation and updates.
- On button devices in landscape, the reader menu's font, font size, alignment, spacing, and screen margin controls now show the live text preview in a full-height column beside the controls instead of a strip too short to show any text.
- Give touch-device line/word spacing and screen margin previews more room with labels beside the sliders and shorter drawers.
- Give the reader spacing and screen-margin sliders more breathing room above the button hints.
- Measure the top margin in global screen-margin previews from the bottom of the header bar, removing the extra gap.
- Display the "Sleeping..." toast on sleep.
- Retry a saved Wi-Fi network found by scanning after the initial automatic connection fails, before asking you to select a network.
- Restore Loading feedback for slow book opens, including cached books, after a 300 ms delay between loading stages; quick opens stay quiet.
- Smart sync only looks up remote progress under the configured Document Matching method, so a position saved for a different version of the file can no longer move you to the wrong place.
- Remove unused button-hint space from Progress Sync screens on touch devices, including landscape.
- Keep words intact across text chunks in long EPUB paragraphs, including non-Latin text and combining marks (#844).
- Keep touch selection advancing onto the next page when a held finger drifts slightly around the final word.
- Add a touch Select button when choosing the Nearby File Transfer receive folder, including empty folders (#850).
- Keep KOReader sync positions inside chapter wrappers deep in long chapters by resolving them before Wi-Fi starts; failed mapping no longer uploads a guessed path (#853).
- Honor EPUB heading and text sizes with installed `.cpfont` sizes on ESP32-S3 readers.
- Center EPUB chapter ornaments inside publisher-styled figures and preserve their spacing beneath chapter titles.
- Use fonts embedded in EPUBs on ESP32-S3 readers, with Off, Ignore Body Font, and All Text modes in global Reader → Font Options below TTF Rendering (when available) and each book’s Font menu below Reader Font.
- Preserve publisher centering and alignment for image-only EPUB paragraphs when reader text alignment is overridden (#856).
- Preserve the selected SD reader font when font details temporarily cannot load, and free Download Fonts catalog memory before restoring it (#855).
- Preserve fine detail when fitting oversized high-color BMP sleep wallpapers and boot images by averaging pixels before dithering.
- Use GitHub Releases for font downloads in builds without a configured custom font host.
- Make Chapter Progress Percentage available in both reader status-bar slot pickers and web settings (#859).
- Restore bottom-edge swipe up to Home outside books on touch devices, including X4 Pro; ordinary list swipes still scroll and reader gestures stay unchanged.
- Accept percentage-only sync positions from compatible servers, including for optimized EPUBs.
- Keep serial logs flowing after low-power USB interruptions (Sung-jin Brian Hong, CrossPoint #3737), disable tilt page turning for accelerometer-only sensors (Justin Mitchell, CrossPoint `8fc2b66`), and discard image caches when their final write fails (Uri Tauber, CrossPoint #3918).
- Include currency and letterlike symbols and the minus sign in newly converted latin-ext SD fonts (Stefan VanBuren, CrossPoint #3801).
- Select the current subchapter when opening EPUB Contents, including chapters with many headings (adapted from Uri Tauber, CrossPoint #3946).
- Allow EPUB lines to wrap after dashes without adding spaces or splitting dash runs (adapted from Phạm Bình An, CrossPoint #3883).
- Center footer button hints using their actual font height (Uri Tauber, CrossPoint #3865).
- Avoid extra temporary allocations when navigating Home’s cover grid (adapted from Sung-jin Brian Hong, CrossPoint #3848).
- Clear the retained sleep image when X3 wakes to Home without a restored Quick Resume frame, including with Sunlight Fading Fix enabled.
- Restore Touch Reader Controls in Settings → Controls → Taps & Gestures so reader touch gestures can be re-enabled on the device after being turned off in the web portal (#871).
- Show all button shortcut settings on the web portal, including option lists too long for the previous response buffer.
- Import CrossPoint's normal 100% word spacing as 0 instead of maximum spacing, while preserving saved CrossInk spacing levels.
- Draw damaged SD-font glyphs as blanks and show a one-time warning to reinstall the font (CrossPoint #4091).
- Avoid large contiguous allocations for EPUB chapter anchors without changing background pagination (anchor-storage portion of CrossPoint #4040).
- Keep the last word before a nested EPUB quotation or heading in its original paragraph; existing book layouts rebuild automatically (adapted from yassine-cc, #886).
- Respond promptly to reader controls while keeping idle power savings, and keep reader plugin bookkeeping from holding up input during a page refresh.
- Prevent a queued navigation click from also repeating after a screen refresh.
- Keep quick physical button taps in order during screen refreshes and filter release bounce before it becomes another click.
- Use stable-page text progress and measured time per stable page for book time-left when available, keeping incremental indexing and font changes from inflating the estimate. Other books retain a chapter-density estimate.
- Credit EPUB and XTC reading to the actual local day and time, preserving streaks when a reading session crosses midnight.
- Preserve reading stats before silent reader restarts, and keep the live session available when a restart save fails.
- Show EPUB and XTC page turns before periodic reading-stat saves, reducing SD-card pauses on button and touch devices. Defer periodic checkpoints until page input and rendering have been quiet for one second.
- Keep words below large decorative first letters intact when EPUB text returns to full width; existing book layouts rebuild automatically (adapted from Uri Tauber, CrossPoint #4173).
- Discover newly added sleep images, including after SD-card copies and USB Drive transfers, and shuffle through every image before repeating. Keep the rotation across sleep and restart, with no immediate repeat between rounds.
- Reduce memory use when opening long bookmark lists from Home or the reader, and avoid a reboot if bookmark loading or the bookmark screen runs out of memory; failed loads and interrupted rewrites preserve saved bookmarks (#892).
- Keep system-folder support files out of Library, including FAT name casing variants, while preserving ordinary TXT and Markdown books and rebuilding existing indexes without deleting source files (adapted from Uri Tauber, CrossPoint #4184).
- Reserve the retained TLS input buffer after sending bulk HTTPS requests, including OPDS book downloads, to avoid later full-record allocation failures when a server ignores the small-record request (adapted from Eszter Schuffert, FreeInk SDK #154 and #155 via CrossPoint #4167).
- Use full refreshes on SSD2677 Sticky displays when previous-frame memory is unavailable, preventing stale black pixels.
- Reduce memory used by OPDS book feeds with long descriptions, release partial catalogs after failed requests, and stop transfers promptly when parsing fails.
- Keep saved tilt preferences when the sensor fails its startup check, while hiding unavailable controls (CrossPoint #4078).
- Fix skipped and repeated plugin catalog entries, including compatibility-filtered static lists, and support encoded item fields in plugin URLs (CrossPoint #4170, #4195, #4196).
- Connect plugin HTTPS requests before copying custom headers to reduce memory pressure during connection setup (CrossPoint #4167).
- Align SD-font text-width measurements with drawing while preserving character spacing; existing EPUB layouts rebuild automatically (CrossPoint #3985).
- Preserve complete non-English characters in Wi-Fi names and masked password previews, and keep installed font rows visibly selectable (CrossPoint #4079, #4106, #4101).
- Load Adobe-obfuscated publisher fonts in EPUBs on devices with scalable font support.
- Ask before setting a sleep cover from the image viewer, and use complete translated delete and sleep-cover questions (CrossPoint #3994, #4097).
- Keep saved EPUB highlights aligned at tight line spacing and preserve letters on adjacent lines (#893).

### Changed

- SD fonts containing both BMP and supplementary Unicode ranges use less resident lookup-table memory, and TTF fonts validate faster while preserving font integrity checks.
- English is the only built-in interface language. Other languages are included in a separate release download and installed through Language settings.
- Display → Hide Battery % is replaced by the Battery option in each status bar's settings; existing choices carry over.
- Home appears faster after waking, closing a book, or leaving a menu by reducing the number of screen refreshes.
- Dictionary lookups and cover thumbnail generation respond sooner.
- Index, open, and resume large EPUBs faster with larger SD writes, quicker SD font preparation (especially for CJK books), and faster chapter, image, and stylesheet discovery. Percentage jumps no longer scan every preceding chapter.
- Monochrome covers and sleep images decode faster while preserving image quality. Large custom sleep and boot images skip rows that are not shown, and oversized boot images are dithered at screen size, matching sleep images.
- Avoid writing reading-percentage caches while browsing Home, Library, or sleep screens.
- Refresh Library cover grids once after missing covers are prepared, keeping input checks between covers.
- Skip text anti-aliasing on EPUB pages without text and avoid copying optimized images twice when they already match the display size.
- Settings → System → KOReader Sync is now Sync Server, grouped into account, What to Sync, and Sync Options sections.
- Reading Stats and Clippings sync stay off until you turn them on under What to Sync. Servers that cannot receive them are detected when you authenticate.
- Rename sync actions to Sync Book and Sync Folder to reflect stats and clippings support, and File Transfer → Sync Stats to Nearby Stats Sync. Upload Stats becomes Sync Books and follows your What to Sync choices; the Reading Stats page's sync button uploads reading stats only.
- Every sync result, including a successful single-book sync, lists overall stats, reading stats, and clippings as Done, OFF, or Not supported.
- Syncing from Reading Stats inside a book returns to that book afterwards instead of Home.
- Use consistent labels for cover options, date/time syncing, shortcuts, and status messages, including "Loading..." across loading screens.
- Sync Behavior's Ask every time is now Ask on conflicts: every sync uploads without asking when the server has no position for the book, reports Already synced when positions match, and asks only when they differ.
- On CrossPoint Sync servers, each CrossInk reader now sends reading progress under its own device id instead of one shared id, so the server can tell your readers apart. Other KOSync servers still receive the shared id.
- Changing Document Matching on the reader or the web settings page now warns first: books get new ids, so the next Sync Books or Sync Folder checks every book again.
- Sync Books and Sync Folder stay on one screen with the current book, a progress bar, and the running counts, instead of flashing a screen for every book. Failed books no longer stop the sync: the final screen lists each one with what failed ("Progress sync failed", "Stats sync failed", or "Clippings sync failed") and why. Use Up and Down to scroll a long list. In Ask on conflicts, choices wait until every book is done and are then shown one after another.
- Sync Book on a single book shows which part failed with the book's title underneath and a result line for its progress, reading stats, and clippings.
- Reduce repeated SD reads when opening EPUBs, browsing Library rows, and preserving reading position through layout changes; reuse temporary TXT indexing scratch without retaining it while reading.
- Exclude unused Arduino libraries and Bluetooth/Zigbee components from tuned firmware builds (adapted from Uri Tauber, CrossPoint #3857).
- Install SD languages and hyphenation packs on PSRAM-equipped readers with small factory data partitions, including X4 Pro, using an SD cache loaded into PSRAM. Startup reuses an open cache file for sequential validation reads. The bootloader, partition table, and firmware slots remain unchanged.
- Remove the redundant Touch Reader Controls setting from device and web settings; Disable Touchscreen remains the full reader touch lock.
- Reduce temporary memory used to prepare SD-font character widths, while keeping the cache up to date as text changes (adapted from CrossPoint #3948).
- Move OPDS Browser into File Transfer, keeping room for Plugins and Settings on Home.
- Save periodic reading-stat checkpoints every five minutes of accepted reading instead of every minute, while keeping session-end, sleep, and restart saves.
- Match menu button directions and front Up/Down labels to screen orientation on X3, X4 Pro, and X4 Classic while preserving tab navigation and reader page-turn preferences. Slider side buttons follow the screen orientation, with the upper button increasing values and the lower button decreasing them.
- Reduce temporary allocations while finding EPUB line breaks and disable unused XML parser input-context buffering (CrossPoint #4126, #4118).
- Revalidate plugin files with content-based ETags so unchanged files do not need to be transferred again (CrossPoint #4193).

- Use themed web confirmations and alerts, accessible file dialogs, and clearer dropdown arrows; preserve upload cancellation and feedback timers (CrossPoint #3984, #4090).

## [v1.6.1] - 2026-10-03

### Added

- Assign a physical-button-only Home/Reader shortcut that returns from nested reader menus directly to the open book, and otherwise returns Home.
- Turn reading stats tracking on or off for the whole device or individual EPUB and XTC books, while keeping saved history and Time Left estimates.
- Assign separate short-press and long-press actions to the Left/Up and Right/Down side buttons; existing side-button layouts migrate to matching individual actions.
- Assign a side-button shortcut to flip the reading screen 180°, alongside clockwise and counterclockwise turns.
- TTF font support on ESP32-S3 devices. Whole-point sizes from 8pt to 22pt will be automatically available.
- Library replaces Recent Books with a searchable book list, and adds various book metadata sort options.
- Assign Library to power, long-press, button-chord, Home-button, or Quick Actions shortcuts to open the book list directly.
- Customize the top and bottom reader status bars separately, including item positions and progress bars, in EPUB, TXT, and XTC books. Each bar can be previewed where it appears while reading.
- Assign actions to upward and downward slides along either screen edge on touch devices.
- View a selected book's reading stats from its Library or File Browser action menu.
- Reset a book's reader settings from the in-reader Settings tab.
- View chapter pages and book progress in the X4 Pro frontlight drawer while reading.
- Add a Cover Grid Home theme on devices with PSRAM, showing the current book and six recently opened books.
- Show small EPUB images within the surrounding text line instead of separating them from the paragraph.
- Sort web portal files by name or size, browse folder images from the preview, and control file dialogs with Enter and Escape.
- OPDS catalogs show category book counts and clearer folder names.
- EPUB headings and whole text blocks can use different font sizes with built-in scalable fonts and TTF fonts on ESP32-S3 devices.
- Add a Display toggle to swap Library and File Browser on the Minimal and Dashboard home screens.
- Choose Clock, Date, Battery, or Empty for the left, center, and right of the Display status bar; dates are also available in reader status bars and follow the selected date format.

### Changed

- Set Power short-press and long-press to Sleep, Wake, or Sleep/Wake separately; holding Power can always wake the device. Chord shortcuts and the home button can also now sleep the device.
- Brightness and warmth gestures now respond while you drag, with longer swipes allowing finer one-point adjustments.
- Text drawing resolves clipping and screen rotation once per glyph, reducing work when painting menus and book pages.
- Library reuses its index on return visits and after waking from sleep, and refreshes after file changes instead of scanning the card every time.
- Home reads saved EPUB progress and chapter metadata without opening or indexing the book, and stops saved-item checks after the first file.
- Optional EPUB background work yields immediately when rendering is busy, keeping input polling responsive.
- SD-card fonts share identical character lookup tables across styles, reducing memory use and repeated card reads.
- EPUB reader menus now share five tabs across devices. X3, X4, and X4 Classic gain live font and margin previews, Reading Stats, and in-book transfer options.
- The on-screen keyboard now uses wider outlined keys with clearer spacing on touch and button devices.
- Long status titles shorten faster when they do not fit the screen.
- Leaving an EPUB or TXT reader releases rebuildable font buffers for other screens.
- Global reader spacing and margin controls now show a live text preview on touch and button devices; the font preview also includes the Latin sample text.
- Font previews now separate the sample text from the font name with a horizontal line.
- Menu buttons now follow the device layout: Left/Right switch tabs and Up/Down select rows in reader menus and global settings.
- The KOReader Sync progress choice now shows remote and local progress as clear cards with large percentages, progress bars, and an "Ahead" marker on the further position.
- Saving unchanged reading statistics and session state avoids unnecessary SD card writes.
- Large secure downloads reuse their receive buffer to reduce repeated allocations.
- The web portal uses less firmware space when the pinned esbuild minifier is available.
- Built-in fonts use less firmware space while preserving their supported characters, ligatures, kerning, and rendered appearance.
- SD-card reads keep filesystem metadata cached separately from book and font data to reduce repeated card access.
- Repeated EPUB progress saves avoid rewriting identical positions and Home percentages.
- Restoring a position in a long EPUB chapter reads its page lookup table in small batches.
- Library searches skip reading completion history for books that do not match the search.
- Library scans count the books found so far and can be cancelled with Back or a tap, keeping the previous Library. A card with more books than the Library can hold now says so instead of reporting a generic failure.
- Library keeps book metadata as each EPUB is read, so a cancelled or interrupted scan resumes where it stopped instead of starting over.

### Fixed

- Touch keyboards keep up with faster typing by polling during screen updates and avoiding extra key-highlight redraws.
- File Transfer choices no longer appear preselected when opened on a touch device.
- EPUB paragraphs now inherit first-line indentation from HTML and body styles while retaining paragraph-level overrides.
- Saved clipping lists now show a scrollbar when more clippings are available below the visible rows.
- EPUB Safe Mode no longer pins inherited fonts and page layout as personal book settings.
- The X4 Pro Home button now steps back through dictionary lookup, chapter selection, and nested settings instead of jumping to Home.
- OPDS downloads now use the first listed author for filename templates when a catalog also lists translators or other contributors.
- Retain the CSS spacing supplied by empty inline spans.
- Improve stability when connecting to Wi-Fi for update checks and KOReader authentication on X4 Pro.
- Crash reports now identify the primary CPU core, show task names when available, preserve both cores' backtraces, and include the firmware ELF hash needed to decode them.
- Release clipping index memory after closing a book or clearing its clippings.
- Keep clipped text, exported excerpts, and chapter titles on complete characters when shortened.
- Keep clipping-selection button hints from covering book text.
- Changing a reader font with incremental indexing now returns after the current reading position is ready, instead of waiting for the whole chapter to be re-indexed.
- Release builds use the pinned PlatformIO core during nested ESP-IDF configuration.
- Adding the sleep moon to the last screen no longer flashes white in night mode.
- Waking the reader skips the intermediate loading icon refresh.
- Screenshot folder names keep complete non-English characters when shortened.
- Longer power-on instructions wrap on the finished update screen.
- Sticky now records periodic heap and PSRAM statistics over its ROM logging path.
- RTL EPUBs use reading-order swipe and tap directions.
- Korean text keeps natural syllable spacing when justified and wraps by word.
- Footnote choices can be selected directly on the reading page, with a list fallback for links without a visible target.
- Changing global font or page layout settings from the pull-down panel on touch devices now updates the open book when it inherits those settings.
- KOReader authentication now rejects unexpectedly large server responses to avoid crashes.
- EPUBs rewritten with alternate XML namespace prefixes now open normally instead of jumping straight to End of Book.
- Global font previews load all letters and punctuation in the Latin sample instead of showing missing-character diamonds.
- Touch keyboard key hit-testing no longer waits for a screen refresh on devices with TTF support.
- Touch keyboards show consistent brief key feedback while keeping fast typing responsive.
- Server connection errors during manual stats uploads no longer appear as Wi-Fi failures.
- Folder progress sync follows the global Smart or Ask setting and rebuilds missing book metadata instead of silently skipping saved progress.
- Skip a failed book during folder sync and continue with the remaining books, with failed books counted in the summary.
- Carousel Home screen reuses cached cover artwork after reading and prepares other positions only when viewed, while keeping progress, reading time, and menu choices current.
- Sleep entry skips unnecessary pauses. PSRAM devices show independent sleep screens before saving reader progress, and Direct grayscale images skip an unused decode.
- Renaming books in the web portal preserves reading progress, stats, bookmarks, clippings, reader settings, and cached content.
- Side-button Orientation Aware, optional keyboard layouts, and the XTC reader's touchscreen and reading stats settings now show toggle switches instead of Yes/No or On/Off labels.
- In-book setting choices now open with the current value highlighted and visible, including fonts and font sizes.
- Changing the Home theme or UI scale through the frontlight drawer no longer leaves overlapping covers or stale controls.
- Fix filename-based KOReader sync failing at split-chapter boundaries in optimized EPUBs that already contain sync mapping data.
- Quick Resume wakes without a full-screen clearing flash when the saved page can be restored.
- Discard stale SD-card cache contents after a failed read to prevent incorrect reads or later writes.
- Back and Home can cancel an active HTTP book or font upload and remove its incomplete file.
- BMP images with extended headers now use the correct palette colors, and incomplete palettes are rejected.
- Touch gestures remain responsive while File Transfer is serving requests.
- Closing a settings submenu restores its row and scroll position.
- The image viewer redraws correctly after menus and the frontlight panel close.
- Previous-page and chapter shortcuts do nothing when already at the beginning of an EPUB or XTC book.
- Reader shortcuts and the frontlight panel save the current reading position before flows that can restart the device.
- Short upscaled images are cached after decoding instead of being repeatedly decoded on refresh.
- OPDS asks before replacing books, validates completed downloads, checks available space, and preserves the old book if a transfer or replacement fails.
- Holding Power to wake from sleep no longer also opens Quick Actions or runs a reader shortcut.
- Recently opened Library books appear without waiting for a full scan, and books with saved history remain in the recent list.
- USB Drive restarts preserve the frontlight state.
- German date-setting labels are translated.
- EPUB metadata indexing uses bounded scratch memory for books with very large chapter counts.
- OPDS pagination returns to the parent catalog correctly, and rename dialogs keep file extensions outside the initial selection.
- Correct X3 battery capacity calibration when the fuel gauge still uses its factory default or an oversized learned capacity.
- Reopen EPUBs on the last page read after following in-book links, and retain the three most recent Back destinations across closing, sleep, and KOReader sync. Transient footnote previews still return to their immediate reading page.
- On X4 Pro and Sticky, Library indexes up to 32,767 books instead of failing on cards with more than 4,096.
- On X3 and X4, Library also indexes up to 32,767 books. Sorting uses the SD card once a library outgrows a fixed memory buffer, so a large library uses no more memory than a small one.
- Keep EPUB text visible after changing TTF rendering options through the frontlight drawer settings.
- Preserve reading time already accumulated when turning off EPUB or XTC reading stats.
- Editing reading stats dates from the frontlight drawer or the XTC reader menu no longer counts the current session's reading time twice, and closing the drawer keeps this session's page turns and reading pace.
- EPUB text re-lays out after changing TTF rendering options from the frontlight drawer even when leaving with the Home/Reader shortcut.
- Support the second X4 Classic display panel variant.
- The frontlight drawer stays within the screen at large UI scales.
- Nested EPUB paragraphs and blocks keep inherited bold and italic styles.
- Sync Progress from the File Browser shows one loading notice and returns to the same folder and selection.
- Keep saved clock and date status-bar positions when the clock is temporarily unavailable at startup.
- Stay in Settings if there is not enough memory to open TTF Rendering.
- Translate month names in Library date headings and device dates.

### Security

- OPDS and download logs hide URL tokens, fragments, and embedded credentials.

## [v1.6.0] - 2026-09-21

### Added

- EPUBs with stable page numbers can jump directly to a specific stable page from the reader menu.
- Hidden folders can be created using the web file manager now when prefixed with a dot.
- Choose whole numbers, one decimal, or two decimals for the book progress percentage in status bar settings.
- Two-finger Screen Rotation can be turned off in Settings > Controls > Taps & Gestures on multi-touch devices.
- Go to % and Go to Stable Page use a numeric keypad for typing an exact destination, including decimal percentages. Touch devices use the keypad exclusively; button-only devices keep the slider by default and hold Confirm/Select to switch to the keypad.
- Files can be renamed from the File Browser action menu while keeping reading progress, bookmarks, clippings, and recent-book entries linked to the new name.
- Firmware builds can include only selected UI languages to reduce flash usage while preserving English fallback.

### Changed

- PNG, XTC, and image-dithering scratch buffers use fewer heap allocations to reduce fragmentation.
- The shared settings catalog keeps its initial allocation instead of retaining unused vector capacity.
- SPI SD-card transfers are batched through the ESP32 hardware FIFO for faster reads.
- SD-card font prewarming releases temporary lookup buffers before allocating large glyph bitmaps.
- UC8179 grayscale images use a slightly longer waveform for stronger midtone separation.
- EPUB image preparation writes extracted data in chunks and reuses two cached images on PSRAM readers.
- Font menus and the web portal use a persistent catalog that loads one family's details at a time, preventing crashes with larger font collections.
- Web portal pages reuse browser-cached content after checking for firmware updates.
- Rapid queued EPUB page turns defer text anti-aliasing and image loading until the final page, making intermediate turns faster.
- Grayscale sleep screen images use the panel's direct grayscale waveform where supported, which folds the base frame into the grayscale pass instead of refreshing the screen separately first.

### Fixed

- The web EPUB optimizer now accepts books that use standard Adobe or IDPF font obfuscation, while leaving DRM-protected books unchanged.
- Frontlight schedule time pickers now use the compact number keypad from Go To screens.
- X4 Classic's left/right tilt direction labels now match the physical page-turn direction.
- Touch keyboards no longer show button-only hold and navigation hints.
- The web settings page no longer offers the Up + Down shortcut on devices that cannot use it.
- OPDS Wi-Fi selection and search entry stay awake while the user is actively choosing or typing.
- USB Drive exits cleanly when a connected host is unplugged without ejecting first.
- EPUB ordered lists show numbers, respect marker-free styles, and retain their container indentation.
- EPUB chapter layout releases rebuildable font caches first, reducing low-memory failures on X3/X4.
- KOReader Sync uploads retain exact text-node positions, including zero offsets and UTF-8 text.
- Saved clipping highlights now retain Focus Reading's custom-font glyphs instead of showing replacement characters.
- EPUB dictionary lookup can select an individual part of a hyphenated word.
- Short Power-button frontlight and touchscreen shortcuts in EPUB books no longer run the configured long-press action.
- Silent restarts now preserve the frontlight state instead of applying wake or schedule settings.
- The Home button now returns from Status Bars to the previous menu instead of leaving the reader.
- OPDS book downloads can follow secure redirects without sharing catalog credentials with the download host.
- Larger EPUB stylesheets work on PSRAM readers, including rules that hide duplicate images.
- JPEG-heavy EPUBs can use PSRAM for decoding on supported readers, leaving internal memory available for reading.
- Importing CrossPoint settings preserves tap and swipe modes without carrying over a stale reader touchscreen lock.
- Saved clippings no longer highlight unrelated single words at page boundaries when matching text after a layout change.
- Quick Lock sleep now respects the configured short Power-button wake behavior.
- Quick Lock now clears when the device wakes after an automatic sleep timeout.
- EPUB content marked with the HTML hidden attribute no longer appears in the reader.
- EPUB paragraphs without source indentation no longer gain a synthetic first-line indent.
- End-of-book selection remains consistent during concurrent redraws.
- Image dithering reports low-memory failures instead of aborting during buffer allocation.
- The debugging monitor plots CrossInk heap and PSRAM logs separately; ZIP failures identify the affected EPUB entry.
- Many progressive JPEG images that store brightness and color in separate scans now render instead of appearing blank.
- PNG sleep overlays preserve four evenly spaced grayscale levels on supported displays.
- Exiting Calibre Wireless on X4 now returns Home with one clean screen refresh instead of repeated blank flashes.
- Manage Fonts no longer crashes after Wi-Fi connects on ESP32-S3 readers.
- Editing font settings from the top drawer's global settings within a book now applies those changes when no per-book font settings exist.
- Per-book reading stats now write to a backup file first.
- Paragraph-alignment previews remain available on text-heavy pages instead of disappearing when the preview sample is full.
- Quick Actions assignments stay visible in button-combo settings, and X4 Classic can use the Up + Down shortcut.
- Sync Progress from the reader menu opens KOReader setup when credentials have not been configured.
- Button-combo settings no longer offer Sleep because the same combo cannot wake the reader.
- EPUB variation selectors no longer appear as missing-glyph boxes after otherwise supported symbols.
- Cancelling Word Spacing on button readers no longer briefly changes the slider value.
- The File Browser now displays decomposed Hangul and accented filenames copied from macOS correctly.

## [v1.5.1] - 2026-09-10

### Added

- Xteink X4 Pro and X4 Classic support, including device-specific firmware and USB Drive access; X4 Pro also supports direct USB file transfers.
- The built-in EPUB optimizer can keep cover art in color while still resizing it to a reader-safe baseline JPEG.
- Custom BMP boot screens, selected in the File Browser or rotated from `/bootscreen` or `/.bootscreen`; sleep screens can also be selected from any folder.
- Quick Lock, assignable button combinations, and shortcuts for Previous Page and Nearby Position Sync. Quick Actions can also be assigned to Power + Up and X4 Pro Home-button gestures.
- Configurable touch page-turn gestures, pinch-to-resize text, two-finger rotation and swipe actions, and a tap-to-hide reader status bar.
- Selectable keyboard layouts, switchable from the keyboard's language key.
- Clippings from dictionary lookups on touch devices, plus selection of text inside EPUB tables.

### Changed

- Touch EPUB readers use a half-height, five-tab menu. Sticky opens the menu with a swipe up and book details with a swipe down; X4 Pro frontlight controls include reading stats and reader shortcuts.
- Screen margins have separate Top/Bottom and Left/Right controls, adjustable up to 200 pixels.
- Night Mode applies system-wide on ESP32-S3 devices; frontlit readers can disable periodic full-screen refreshes.
- Waking keeps the sleep screen visible until the reader or Home is ready, unless a custom boot screen is enabled.
- Font choices show available point sizes, Download Fonts replaces the font-manager label, and Wi-Fi passwords are visible during entry.
- Reader controls, shortcut pickers, touch targets, and File Browser settings are easier to reach; Book Options is last in the button reader menu.
- EPUB indexing, image decoding, fonts, and reading-state updates use fewer resources; the web optimizer prefers natural boundaries when splitting chapters.

### Removed

- The undocumented X4 Pro power-button double-click frontlight toggle.
- Built-in reader-font emoticons and hand gestures; SD-card fonts retain emoji fallback support.

### Fixed

- Clipping highlights stay aligned after font changes, retain multi-paragraph text, and remain readable in Dark Mode. Selection stays on its final page, and browsing saved clippings responds reliably.
- Dictionary lookup respects landscape controls and selected fonts, handles repeated lookups more reliably, and returns to the reader cleanly when dismissed.
- EPUB tables retain column widths and wrap long labels; mixed-direction text, Arabic/Persian shaping, ruby annotations, and footnote styling render correctly.
- EPUB contents links, split-chapter navigation, footnote resumes, and end-of-book exits preserve the intended reading position.
- Large EPUBs, image pages, and SD-font preparation recover more safely from limited memory and SD read errors.
- XTC/XTCH page turns no longer overlap, tables of contents show all entries, and covers retain their grayscale detail. TXT font-size controls and Home progress work reliably.
- Quick Resume, custom sleep images, transparent overlays, and X3/X4 wake refreshes avoid blank screens, grid artifacts, and lingering images.
- Long-press shortcuts no longer trigger an extra action on release; Quick Actions, Quick Lock, and Dark Mode shortcuts respond consistently.
- Touch scrolling, page gestures, font-download cancellation, and reader settings behave reliably across orientations and UI scales.
- KOReader Sync preserves orientation and settings, handles missing remote positions, and avoids repeated screen flashes during network transitions.
- Nearby sync, OPDS search, file listings, and image actions handle input and errors more reliably; Calibre Wireless shows the full IP address.
- S3 sleep, charger detection, and power-button wake behavior are more reliable. USB Drive recovers from storage failures, USB transfers avoid watchdog errors, and updates reject firmware for a different board.
- Book-specific settings stay separate from global defaults, and Recent Books and KOReader credentials survive network restarts.

## [v1.5.0] - 2026-08-08

### Added

- X4 Pro readers can lock the Home button while reading, with a Power-button shortcut to toggle it.
- End-of-book suggestions can now be opened directly by tapping their rows on touch devices.
- Quick Actions lets readers assign up to five favorite reader commands to one Power, Back, or Menu shortcut.

### Fixed

- EPUB tables now lay out a row at a time in both Incremental and Full Section indexing, keeping regular tables readable without whole-table buffering.
- Touch support for Seeed Studio Sticky
- Nearby File Transfer can send EPUB, TXT, XTC, XTCH, PNG, and BMP files directly between two CrossInk devices without a Wi-Fi network.
- Recent Books and image-file long-press actions can send files directly to a nearby CrossInk device.
- Dictionary lookup and lookup history
- EPUB books can use a dedicated SD-card dictionary font while keeping a different reader font.
- EPUB books can set a dedicated dictionary font size independently of the reader font size.
- Dictionary font and size defaults can be set globally from Settings > Reader > Font Options, with per-book choices still taking precedence.
- Reusable dictionary SD-font builder with IPA coverage and per-family ZIP packaging
- RTC-enabled devices can now choose the date format and numeric separator shown in headers from Settings > System > Device.
- The web EPUB optimizer now splits oversized chapters into memory-friendlier sections before sending them to the reader.
- Reader indexing can now use `Incremental` or `Full Section` mode globally or per book; changing modes keeps the current chapter readable and applies when the next chapter needs indexing.
- Look Up Word can now be assigned to short- and long-press Power button shortcuts.
- EPUB readers can now choose from five word-spacing levels, from normal through extra-wide.
- EPUB inline-image pages on X3 now use the grayscale-aware display base before the image grayscale overlay, reducing the moment where images appear too dark before settling.
- EPUB publisher small-caps styling now renders ASCII lowercase text as smaller capital letters without needing extra font files.
- When incremental EPUB indexing runs out of memory at the first unindexed page, the reader now silently restarts once and resumes the book with a fresh heap.

### Changed

- PSRAM-equipped readers now keep EPUB grayscale and image-cache working buffers in external memory, preserving more internal RAM for layout and reducing repeated SD reads on image pages.
- Reader font sizes now persist as actual point sizes, keeping the closest matching size when font families or installed files change.
- SD-card fonts now include the built-in reader fallback stack for common symbols, emoji, and selected CJK glyphs while retaining Noto Sans fallback coverage.
- Downloadable SD-card fonts are now rendered with the same darker anti-aliasing as the built-in reading fonts.
- Full-section EPUB indexing now prepares one-page chapters and direct jumps to a chapter's last page, while avoiding repeated checks after the next chapter is ready.
- EPUB grayscale rendering now reuses its 8 KB strip buffer across stable pages, reducing repeated heap allocation and release during long reading sessions.
- Reading progress is now saved in batches during ordinary page turns, immediately after layout changes, and when leaving a book, reducing repeated SD-card writes without carrying stale pagination into the next session.
- SD-card font discovery now waits until a custom font is selected or font settings are opened, reducing SD-card work during normal startup with built-in fonts.
- EPUB page turns using SD-card fonts now prepare the next page's glyphs while the reader is idle.
- Dictionary lookups now reuse open index files for stem matching, reducing repeated SD-card work after a miss.
- The web file manager now batches directory listings into fewer network packets, improving large-folder response time.
- Firmware releases now identify the supported device type: X3/X4 or Seeed Sticky.
- Image-heavy EPUB chapters now index by reading image headers first and extract each full image only when its page is shown.
- EPUB books with repeated byte-identical stylesheets now parse each unique stylesheet only once when building caches.
- SD-card fonts now reuse their page-sized glyph buffers, reducing heap fragmentation during long reading sessions.
- Firmware builds now prioritize usable heap over oversized system timer stacks and maximum WiFi throughput, leaving more memory for reading and network operations.
- Downloaded-font size range options now show their actual point-size ranges instead of firmware build names.
- KOReader Sync and authentication, OTA updates, and OPDS browsing now restart into a lightweight network mode that leaves reader and Home data unloaded, providing more contiguous memory for WiFi and secure connections.
- The web file manager can now delete non-empty folders recursively and, when hidden files are shown, remove hidden or system-managed SD card items after confirmation.
- SD-font, OPDS catalogs, and other unneeded settings now stay out of memory while reading unless their settings are open.
- EPUB books can now keep more saved clippings without loading every clipping's text into memory while reading.

### Removed

- The font download manager no longer offers a Download All action; fonts can still be downloaded individually or updated together.

### Fixed

- Book menu tab navigation, popup scrolling, customized Reading Stats hints, and short button presses after low-power mode now work reliably.
- Sleep screens now honor the current orientation, avoid X4 transition flashes, fall back to a valid wallpaper when needed, and handle low-memory image decoding without rebooting.
- Choosing Set Cover uses the selected image in place, and Home no longer repeatedly generates missing EPUB covers.
- Finished-book suggestions are now collected before an EPUB is moved to `/Read`.
- Manage Fonts now opens and scans large catalogs more safely on X3/X4, reports low-memory failures instead of restarting, and returns to Font Options when cancelled.
- Network screens refresh cleanly on X4; long errors wrap correctly; saved Wi-Fi networks and KOReader connections recover more reliably after restart or a missing address.
- Translated Wi-Fi and clock labels no longer truncate text or time values, and clock sync no longer risks a reboot while saving settings on memory-constrained X3/X4 devices.
- KOReader Sync no longer crashes during time setup, re-triggers while connecting, or loses precise EPUB positions; CrossPoint-only data stays on the official CrossPoint Sync server.
- Firmware updates reject images for the wrong chip family, and saved Wi-Fi settings safely handle concurrent access and corrupted values.
- EPUB opening, reflow, and background indexing now handle fragmented memory more safely, retry recoverable work, remain responsive to input and setting changes, and show useful errors instead of rebooting or silently returning Home.
- Low-memory EPUB grayscale and sleep rendering now fall back safely without leaving stale display content.
- Full-section indexing preserves more memory for large chapters and cancels speculative work on page turns, keeping the reader responsive.
- SD-card font and clipping work now release temporary data at the right time, preserving memory for reflow, dictionary use, covers, and thumbnails on X3/X4.
- EPUBs with book-specific built-in fonts no longer load an unnecessary global SD-card font, and custom fonts retain ligatures.
- EPUB styling choices apply before style caches load; CSS-heavy books use less temporary memory; and disabling Embedded Style consistently skips unused stylesheet work.
- EPUB layout now keeps CJK ruby and spaces, Russian paragraph continuations, Focus Reading, underline/strikethrough runs, and right-to-left text correct.
- EPUBs with flowing `<br>` elements, image-led or decorative chapter headings, unsupported images, and dense final pages now lay out without excess gaps, clipping, dropped images, or misleading low-memory warnings.
- EPUB footnote and cross-reference previews now show complete notes, including targets in the middle of a paragraph.
- Saved EPUB positions, clipping highlights, and selections now stay accurate after font, orientation, or indexing changes; selections also remain readable in dark mode and on memory-tight pages.
- Dictionary misses can switch dictionaries without leaving the reader, and dictionary read failures now report an error instead of a false “not found.”
- Reader popups, KOReader Wi-Fi labels, Lyra battery headers, and the sleep message now remain correctly oriented and positioned.
- Manual refreshes preserve EPUB and TXT text anti-aliasing; XTC and XTCH status bars show the configured time-left estimate.
- Watchdog panics with captured diagnostics open crash reporting, while reset-only events return normally; power-button wake timing no longer depends on SD-card startup.
- The web file manager and uploads now handle simulator/device ports and stalled connections safely; unsupported settings stay hidden, and the optimizer removes empty chapter stubs without breaking table-of-contents links.

## [v1.4.0.1] - 2026-07-28

### Added

- Updates to support Xteink device detection so the correct display panel driver is used.

## [v1.4.0] - 2026-07-10

### Added

- Dashboard UI theme for the Home screen, showing the current book cover and reading stats.
- Nearby Position Sync for sending or applying the current EPUB position between two CrossInk devices over ESP-NOW.
- Web EPUB optimizer support for CrossInk location metadata, so optimized EPUBs can keep better progress and stable page numbers.
- Reading Stats support for XTC and XTCH books, including reader menus, Home and sleep screen stats, mark finished, delete stats, and preserving stats when clearing book caches.
- Web file manager image previews, so PNG, JPEG, BMP, GIF, and WebP files can be viewed inline before downloading.

### Changed

- Large EPUBs, SD-card font-heavy books, and cover thumbnails now open, index, and generate more reliably under low-memory conditions.
- Home and sleep screens now load more cover and thumbnail data only when needed, reducing reader startup work and reusing cached cover data where possible.
- Built-in reader font choices have been reduced to Lexend Deca and Bitter, reducing firmware size while keeping fallback glyph coverage.

### Removed

- Teensy firmware builds are no longer produced for releases or release candidates.

### Fixed

- EPUB render-mode and Safe Mode toast messages now clear reliably, even when the reader is low on memory.
- EPUB Reading Stats no longer drops unsaved page-turn counts after viewing the stats screen mid-session.
- KOSync is more reliable with many SD-card fonts installed, reducing low-memory failures during secure sync requests and uploads.
- Web file manager actions now handle filenames with special characters safely and reject unsafe rename characters before saving.
- Auto Turn interval settings and related action prompts opened from long-press shortcuts now stay open after releasing the shortcut button.
- EPUB footnote previews no longer show clipped status-bar labels or misleading reader progress indicators, and clipping selection now works from footnote previews.
- Font selection no longer reopens the font preview after choosing a font.
- EPUB chapters with stale publisher style data now rebuild it instead of opening without the book's styling.
- Large SD-card font EPUBs no longer overlap characters after font or line-spacing changes, and clipping selection can fall back to a built-in UI font when needed.
- EPUB cover and thumbnail generation is more reliable with custom SD-card fonts selected and optimized books under low-memory conditions.
- Web EPUB optimizer now preserves more PNG and SVG artwork on-device, including transparent PNGs, dividers, and images in malformed or XML-declared chapters.
- Unsupported SVG images in EPUB chapters are now skipped silently instead of triggering low-memory image warnings.
- Nearby Position Sync now silently restarts back into the reader after using ESP-NOW, matching other WiFi sync flows and reducing post-sync memory fragmentation.
- EPUB page cache loading now uses fewer small heap allocations, reducing fragmentation-related reader failures.
- EPUB grayscale page turns on X3 now use the grayscale-aware display base, reducing the moment where new text appears too dark before the anti-aliased overlay finishes.
- EPUB chapters with many inline anchors, footnote links, malformed XHTML, large publisher styles, or SD-card fonts are less likely to fail or get stuck on the indexing screen.
- EPUB opening and image rendering now recover from more low-memory conditions instead of rebooting, including landscape image pages and books that need lighter render modes.
- EPUB clipping selection now follows right-to-left line order when selecting Hebrew and other RTL text.
- Lyra Carousel no longer shows a blank carousel after returning from WiFi-related File Transfer screens and moving between the menu row and book row.
- Generated SD-card font packages now include the same core glyph coverage as built-in reader fonts.
- Manage Fonts no longer crashes while loading or reloading large SD-card font lists.
- Minimal Home no longer swaps to another recent book when returning from Settings when Back button is mapped to the first button.
- Cancelling a font download now stops on the first Cancel button press instead of needing several presses.
- The `Inverted` sleep cover filter now keeps book covers unchanged on Minimal and Dashboard sleep screens while switching the background to white.
- Rare EPUB open or thumbnail crashes during ZIP decompression are fixed.

## [v1.3.4] - 2026-06-24

### Added

- File Browser now indexes large SD-card folders so directories with many books can be browsed without loading every filename into memory at once.
- EPUB text clipping with saved highlights, clipping lists, and Kindle-style `/My Clippings.txt` export.
- `Create Clipping` is now available as a reader shortcut for short/long Power, long-press Menu, and long-press Back actions.
- Per-book EPUB options for font, layout, styling, reading aids, and render modes, including `CrossInk Default`, `Balanced`, and `Light` modes for difficult books.
- Arena allocator (`lib/Memory/Arena.h`) for burst-then-discard allocation patterns - reduces heap fragmentation during EPUB parsing and page layout over long reading sessions.
- Optimized EPUBs now store location metadata at `META-INF/x-locations.json`.
- X3 SD-card writes now use the RTC for file timestamps when the clock is available.

### Changed

- The EPUB reader menu now splits the growing menu into 3 screens, labels per-book settings as `Book Options`, and avoids showing duplicate `Orientation` controls.
- The `Inverted` sleep cover filter now flips Minimal and Reading Stats sleep screens to black text on a white background.

### Fixed

- Quick Resume no longer shows a blank page after EPUB next-chapter indexing.
- Calibre Wireless transfer status no longer stacks the last received-file message on top of the upload percentage.
- X3 Tilt Direction now labels left/right choices as `Left-Right` and `Right-Left`, with existing left/right preferences migrated to keep the same physical tilt behavior.
- EPUB layout now honors publisher page-break CSS, avoids stretching justified spaces before closing punctuation, and keeps large CSS rule sets in a smaller disk-backed lookup cache.
- EPUB first-open conversion now uses more compact OPF manifest lookups and streams cover-wrapper parsing to avoid large temporary heap buffers on books with huge manifests.
- EPUB chapters that run out of memory now retry with `Balanced`, `Light`, and final `Safe Mode` rendering before showing an error, apply the same fallbacks during next-chapter pre-indexing, and let book action menus reset a book's reader settings if Safe Mode still cannot open it.
- EPUB reader font-size changes now restore the current chapter position by content instead of jumping far backward after re-indexing.
- Reading Stats now use the reader's last live book time-left estimate instead of showing a separate fallback estimate.
- Per-book reading stats now migrate compatible legacy `stats.bin` files into the `stats_v5.bin` flow instead of resetting when only the old filename exists.
- Lyra Carousel Home menu rendering now avoids extra label allocations that could crash builds under low memory.
- Lyra Carousel Home cover refresh no longer risks a reboot when memory is tight after returning to or selecting a recent book.
- EPUB image-heavy chapters no longer risk a reboot while saving their reading cache under low memory.
- TXT readers now stay open when pressing a page-turn button at the end of the file.
- Long-press reader shortcuts that open another screen no longer close or confirm it again when releasing the shortcut button.
- RoundedRaff's header battery icon and percentage now sit lower to avoid clipping at the top edge.
- Lyra Carousel now keeps the Home header current when rendering the menu or restoring cached carousel frames, preventing stale battery and clock values while navigating between books.
- Web file manager multi-delete now handles larger selections without failing after a small batch.
- Portuguese EPUBs now use Portuguese hyphenation rules instead of leaving long words unhyphenated when Hyphenation is enabled.
- Progressive JPEG EPUB covers now render more smoothly in generated cover and thumbnail BMP assets.
- EPUB section layout now flushes long text runs earlier when Focus Reading or Guide Dots are enabled, reducing low-memory failures on difficult books.
- Footnotes in EPUBs with very large shared notes sections no longer cause long stalls when opened.
- Firmware updates now follow GitHub asset redirects before streaming the install.
- Tiled grayscale rendering now serializes display transfers on the shared SPI bus to avoid display glitches during SD activity.

## [v1.3.3] - 2026-06-13

### Added

- `File Browser Display` in `Settings > System > Files & Cache` for choosing one-line or two-line file browser rows across all themes, while preserving Minimal users' existing two-line display on upgrade.
- `Hide File Extension` in `Settings > System > Files & Cache` for expanding file-browser filenames by hiding the right-side extension label.
- Device Name in Settings > System > Device for customizing the KOReader Sync and Nearby Stats Sync device label.
- Additional shortcut options and new ability to add custom shortcuts for Long-press Back Action.
- Delete Reading Stats actions in the EPUB reader and book action menus for clearing one book's stats without deleting its cache.

### Changed

- CrossInk settings now save to `/.crosspoint/crossink-settings.json`, with a one-time fallback migration from `/.crosspoint/settings.json`, so switching between firmware builds is less likely to reset preferences.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.

### Fixed

- RoundedRaff's date shown in settings now sits lower on X3 devices instead of overlapping the battery.
- Clear Bookmark List now asks for confirmation before deleting a book's bookmarks.
- Clear Reading Cache now preserves per-book reading stats while continuing to leave all-time reading stats untouched.
- Moving finished EPUBs to `/Read` now consistently preserves reading progress, per-book stats, bookmarks, and resume state.
- Book settings option lists now return to the submenu they were opened from when pressing Back.
- Lyra Carousel now refreshes its cached Home icon row after OPDS, Reading Stats, or Bookmarks icons appear or disappear.
- KOReader Sync failure screens now wrap long error messages and shut down WiFi cleanly before returning to the book.
- Sleep Screen > Cover now generates the current book cover on demand instead of falling back to the dark sleep screen when the setting is changed after opening a book.
- File Browser now previews PNG images instead of trying to open them as EPUBs, and hides common macOS and Windows metadata files.
- File Browser now refreshes immediately after falling back to the root folder from a stale saved path.
- File Browser now stops loading oversized folders before low memory can crash the device and shows a memory error instead.
- TXT reader long-press Power page turns now work when Long Power Button is set to Page Turn.
- SD-card font read failures no longer risk a reboot while cleaning up the failed file read.
- Page Overlay sleep screens no longer force EPUB chapters to re-index after waking.
- Page Overlay sleep screens now use the current screen as the overlay background outside the reader instead of trying to rebuild a stale book page.

## [v1.3.2] - 2026-06-10

### Added

- Current date in the top-right Settings header on X3 devices.
- Dark Reader Mode for EPUB and TXT reading screens, plus shortcut actions for the power button and front-button long press.
- File Browser long-press folder action for choosing a custom sleep-image folder instead of only `/.sleep` or `/sleep`.
- Expanded X3 Reading Stats, including streaks, time charts, editable dates, all-time backups, reset controls, an idle-time threshold, and the `Minimal Stats` sleep screen.
- `Reset Reading Pace` in the EPUB reader menu when Time Left is enabled, for clearing only the time-left pace estimate while keeping book reading stats.

### Changed

- Display, Reader, and Controls settings now open list menus instead of cycling through options one by one.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.
- Reading time and time-left pace tracking now ignore page intervals longer than the configured idle-time threshold.
- Web portal pages now use shared templates, stylesheet, and logo assets, reducing on-device page size and improving browser caching.
- Already-cached EPUBs now open directly to the first page without an extra book-loading popup refresh.
- Reader font-size choices now show point sizes like `10 pt` instead of names like `Tiny`.

### Fixed

- Inverted reader menus now honor orientation-aware side-button navigation.
- EPUB book time-left estimates now wait for more session pace samples and use a progress-based floor after pace data exists, reducing swings from unusually short or long pages.
- Deleting an EPUB book cache now preserves that book's reading stats and pace data.
- X3 clock settings now have clearer UTC offset editing, and `Sync Date/Time` can use saved WiFi networks automatically.
- Home, Lyra Carousel, WiFi setup, and SD-card font flows now release memory more aggressively to avoid freezes or crashes on constrained builds.
- Vietnamese settings labels no longer show replacement diamonds after generated translation offsets shifted.
- KOReader Sync now lands correctly at chapter starts and shows more specific connection guidance.
- EPUB bookmarks saved under the old unstable path hash now show up again, including for books moved to `/Read`.
- SD-card font downloads now use versioned direct S3-hosted HTTP endpoints with CRC validation, avoiding GitHub release redirects and ESP32-C3 TLS stalls when loading the font catalog.
- EPUB text blocks now keep the book's alignment style when an inline image appears before the text.

## [v1.3.1] - 2026-05-28

### Added

- EPUB reading-position improvements, including bookmark anchors, bookmark preview snippets, and optional chapter/book time-left estimates.
- Nearby Reading Stats sync with separate totals for this device and all synced CrossInk readers.
- Per-server OPDS filename settings so downloaded books can use either Author - Title or Title - Author.
- EPUB render heap diagnostics that include the largest allocatable block, not just total free heap.

### Changed

- Moved the X3 reader clock into a new top-centered status bar and moved clock settings to Settings > System > Device.
- Reworked Display, Reader, Controls, in-reader options, and larger System settings groups so related options open as submenus.
- Improved OPDS and font download responsiveness by reducing progress-update overhead and temporarily disabling WiFi power saving during transfers.
- Book selection now shows a loading popup before EPUB indexing or cache loading begins.
- Delayed the automatic finished-book prompt until the reader leaves the chapter where they reach 99%.

### Fixed

- WiFi settings screen now keeps the displayed MAC address consistent with the router-visible WiFi address.
- Reader UI issues with inverted menu button hints, Lyra Carousel popups, and Auto Page Turn interval persistence.
- Web uploads and KOReader Sync progress saves now preserve progress, stats, settings, and valid resume data for refreshed book files.
- OPDS low-memory handling now shows a specific parser-buffer memory message and releases SD-card fonts before catalog loading.
- EPUB cache, CSS, table, SD-card font, and allocation failure paths now recover, retry, or stop cleanly under low memory instead of opening unstyled pages, failing unnecessarily, or risking a reboot.
- EPUB text with invisible word-joiner characters no longer shows replacement diamonds for missing font glyphs.
- Clarified the low-memory EPUB image warning so it says some or all images may be missing.

## [v1.3.0] - 2026-05-21

### Added

- Back/Cancel support while downloading books from OPDS catalogs.
- Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- More detailed WiFi connection debug logs for scans, selected networks, status changes, disconnect reasons, and timeouts.
- 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.
- In-reader confirmation message when a shortcut turns tilt-to-turn on or off.

### Fixed

- WiFi and OPDS connection-flow edge cases: manual Settings connections now show the connected status before continuing, copied or corrupted saved-password files are rejected before use, OPDS retries show loading before requests, and large OPDS feeds fail safely under low memory instead of rebooting.
- Reader and Home UI polish issues, including landscape status-bar settings, missing Vietnamese labels, File Browser and Lyra Carousel icon alignment, cover thumbnail artifacts, and duplicate Home progress/stat loading.
- EPUB cache and low-memory handling now use stable cache folder keys, migrate older cache folders where possible, rebuild stale section caches, lay out very long text blocks earlier, stream table fallback content when heap is tight, and clarify the warning text.
- Sleep-entry, network, and SD-card font download reliability improvements: cached sleep-screen assets are reused, OPDS pages idle normally after load, the X3 tilt sensor sleeps outside the reader, WiFi power saving is disabled during transfers, WebDAV stack usage is lower, longer stalls are tolerated, interrupted font files are retried, and active reader fonts are freed when needed.
- Remaining reader service edge cases, including an XTC chapter selector crash on memory-constrained builds, SD-card font size selection, SD-card font-size shortcuts skipping manually installed sizes, and KOReader Sync login compatibility with self-hosted servers that return valid JSON on success.

### Changed

- Modified upstream "page-as-sleep" behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Improved reader and browser menu behavior by moving the Footnotes shortcut above Select Chapter, wrapping long book titles in action menus, and reducing progress-screen repaint work during OPDS and SD font downloads.

## [v1.2.11.1] - 2026-05-15

### Changed

- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed

- Lyra Carousel is now included by activating the build flag `DCROSSINK_ENABLE_LYRA_CAROUSEL=1`

---

## [v1.2.11] - 2026-05-14

### Added

- New personal theme: "Minimal"
- Custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- In-reader Controls shortcut for customizing buttons without leaving the book.
- Bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Confirmation message after deleting a book's cache from the reader or File Browser.
- File Browser long-press action for deleting an EPUB or XTC book's cache.
- Downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed

- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed

- Inline EPUB images no longer disappear in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- SD-card font picker no longer reopens immediately after selecting a font from Settings > Reader > Font Family.
- In-reader font-size changes now work for SD-card fonts.
- In-reader SD-card font changes now rebuild the current EPUB page layout consistently.

## [v1.2.10] - 2026-05-11

### Added

- `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- More flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Per-session auto page turn interval picker with values from 5 to 120 seconds.
- File Browser Home/Back long-press action for toggling hidden files and folders.
- EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed

- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed

- Reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Font and glyph handling now reduces persistent SD-card font advance-cache memory, releases optional font caches before image extraction only when heap is tight, and shows a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- EPUB low-memory and cache safety now skips optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, fails safely with a malformed-book warning and Home exit path, rebuilds incompatible fork-written caches, and handles low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Home no longer crashes after clearing reading cache when the source EPUB cache is missing.
- Reader prewarm behavior now skips image decoding, keeps mixed-style font glyphs cached together, and avoids section rebuilds for render-quality-only option changes.
- Concurrent render/storage crashes are avoided by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling now keep cached book data in sync and grid covers fill their slots correctly.
- Simulator build configuration now lets SDL2 and simulator-provided network/OTA shims compile cleanly.

---

## [v1.2.9.1] - 2026-05-03

### Changed

- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed

- Power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions.
- Potential crash when using `Go to %` in EPUBs.
- Potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid.
