# Built-in compressed group pressure (X3 firmware QEMU)

This is a stress fixture, not a performance result. No embedded or SD fonts.
Each short unit contains regular body, bold, italic, bold-italic, and a 150% block.
Use the same built-in compressed family and reader size on baseline and candidate.
Verify five distinct compressed `EpdFontData` groups actually enter the page scan:
family aliases, missing faces, fallback fonts, page breaks and available sizes can
reduce the count. A page with fewer groups is a control, not evidence of the fix.

The regular body primer comes before the inline 150% heading, fixing scan order.
The body mixes ASCII and Latin-1 (café), exercising more than one compressed
glyph block if it falls back. Keep the size declaration inline so this fixture
exercises font grouping independently of stylesheet discovery.

Package with Python's zipfile: write `mimetype` first using ZIP_STORED, then the
META-INF and OEBPS files with ZIP_DEFLATED. Do not include this README.

Run the actual `crossink-emulator x3` firmware workflow, not the SDL application
simulator. Compare base 6bce22668 and candidate using separate fresh SD images,
identical font/size, portrait, embedded style ON, normal render mode, same pages.
Capture cold first page and at least five warm adjacent turns. Test AA ON/OFF.
Control: embedded style OFF removes the second-size block; a regular-only chapter
is another control. Keep status bar identical because it adds scanned groups.

Capture page stage timing, FDC hits/misses, compressed prewarm refusal logs,
free/largest heap and framebuffer screenshots. FDC `logStats` includes
`slot_refusals` but resets counters: take one snapshot after all BW/AA passes,
not one snapshot per glyph. If a debug harness calls it, keep that identical in
both firmware variants. Higher refusal count is not itself failure: four slots
remain the limit and omitted groups must render through hot-group fallback.

Expected: dominant body groups remain resident; fallback decompressions shift
to the smaller omitted group, with unchanged pixels, four page slots and no
additional page-buffer budget. The host production-path integration provides the
exact synthetic work-count oracle (360 -> 9 misses across three passes).
QEMU wall time is model timing, not a physical-X3 timing claim. Do not assert a
numeric firmware improvement until the five-group precondition is verified.
