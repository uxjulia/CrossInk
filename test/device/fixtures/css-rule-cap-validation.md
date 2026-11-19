# Actual firmware emulator checklist (not SDL simulator)

Generate with `python3 test/device/fixtures/make_css_rule_cap_epub.py /tmp/css-rule-cap.epub`.
Install in a disposable X3 QEMU SD image with a nonzero FAT modification time;
clear only this book's cache, choose downloaded Bitter, and cold-open then close
and reopen twice. Baseline and patch must share settings and source bytes.

- Cold parse should stop with 1500 retained rules and partial flag 1, terminal flag
  2. X3 may hit the heap guard sooner: that is correctly **transient**, not proof
  of terminal reuse. Use X4 Pro PSRAM firmware to exercise cap deterministically
  if needed; report this distinction, do not fake cap counters on X3.
- On two warm terminal opens, expect `CSS rule-cap cache source unchanged;
  skipping CSS extraction and parse`. No `.tmp.css` extraction/parse/cache rewrite;
  compare CSS cache and layout timestamps/hash. Screenshot all three paragraphs.
- Change source mtime or size, or remove/truncate marker: expect rebuilding log,
  not reuse. Revision-22 header must rebuild once. Do not edit ordinary SD image.
- Under heap-pressure stop or injected extraction/read failure, header stays
  partial-only. Reopen should retry; if still partial, layout caches stay intact.
- Measure per-open time plus CSS parse/extraction log counts. Emulator timing is
  comparative only, not real SD latency. Host tests cover cap state, source stat
  checks with zero source reads, transient read retry, and older-version rejection.
