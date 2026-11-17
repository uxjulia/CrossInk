# Disk-backed CSS selector working set

Generate a fixture (not a firmware simulator run):

```
python3 scripts/generate_css_disk_memo_epub.py /tmp/css-disk-memo.epub
```

The default has 1,000 rules and 800 paragraphs. Only four rules are hot;
unused rules exercise the full stylesheet hydration admission gate. `--rules`
and `--paragraphs` allow smaller controls without editing the generator.

## Deterministic host gate

Build and run `CssArenaBackingTest` with the repository's native CMake suite.
`DiskMemoReusesExactSelectorsWithoutFurtherReads` forces 40 KiB free internal
heap, denying full arena hydration. After warming, 100 repeated style checks
must read **zero** additional CSS-cache bytes. The severe-pressure control
(16 KiB free) keeps the original disk fallback and records its read bytes.
Other tests cover case folding, eviction, long selectors, failed reads, book
isolation, clear, and replacement/reload.

## Firmware emulator / hardware gate

Use the actual X3 firmware emulator, not SDL. For audit builds only, define
`CROSSINK_CSS_PROFILE`. It reports aggregate memo hits, disk-rule read attempts,
and allocated bytes when the parser clears its rules. It does not log each
lookup. Stock builds have no counters. The existing `CSS Rule arena` log must
say **disk index** for this scenario; a hydrated run is not fallback coverage.

Use Book's Style on, Bitter from official font downloads, a fresh layout cache,
and identical font size/margins/orientation for comparisons. Index the fixture,
record section-build time and counters, leave the reader to release the rules,
then reopen and test a settings-driven layout rebuild. Check that page count,
text, paragraph styling, and screenshots match the baseline. Warm finalized
page turns should not need CSS lookups at all.

If the device admits all 1,000 rules, increase `--rules` up to 1,400 or use a
controlled low-heap QA harness. Do not claim fallback coverage merely because
the fixture was opened. If parsing is itself heap-limited, distinguish that
partial-cache retry path from the selector reuse being tested here. Keep the
same populated CSS cache for before/after layout comparisons.

For C3 hardware, repeat with SD timings and free/largest-block measurements.
For S3, verify the normal PSRAM-hydrated path remains unchanged and separately
exercise a rejected hydration case. QEMU times are not physical SD timings.
