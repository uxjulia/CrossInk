# CrossPoint Reader — Consolidated Project Context

## Known Simulator Limitations

- **No image rendering**: `lib_ignore = hal, PNGdec, JPEGDEC` in `platformio.ini`. `ImageDecoderFactory::getDecoder()` returns null → images silently skipped. Would need `stb_image` to fix.
- **JPEGDEC stub**: Always returns failure (`open=0`, `getLastError=-1`). Log message `JPEGDEC fallback: open failed (err=-1)` in simulator is expected and harmless.
- **Deep sleep is a no-op**: `esp_deep_sleep_start()` returns immediately in simulator. `lastActivityTime = millis()` is reset after `enterDeepSleep()` calls in `src/main.cpp` to prevent infinite re-trigger loop.
- **HalStorage**: Uses POSIX `::open()` with `./fs_` prefix; allows multiple readers unlike real hardware (SdFat with mutex, one reader per file at a time).

---

## SdFat: One Reader Per File

On real hardware, SdFat cannot open a second read handle while the first is still open on the same path. Pattern when a fallback needs to reopen a file:

```cpp
jpegFile.close();  // Release handle before fallback reopens it
return fallbackFunction(filePath, ...);
```

See: `lib/JpegToBmpConverter/JpegToBmpConverter.cpp` — `jpegFileToBmpStreamInternal()`.

---

## JpegToBmpConverter — JPEGDEC Fallback

All three public methods require `const char* filePath` (so JPEGDEC open callback can reopen):

```cpp
static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, const char* filePath, bool crop = true);
static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, const char* filePath, int targetMaxWidth, int targetMaxHeight);
static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, const char* filePath, int targetMaxWidth, int targetMaxHeight);
```

JPEGDEC callbacks pattern (matches `JpegToFramebufferConverter`):
- `open(filename, size)`: `new FsFile`, `Storage.openFileForRead(filename, *f)`, return `f`
- `close(handle)`: `f->close(); delete f;`

Callers: `lib/Epub/Epub.cpp:561`, `lib/Epub/Epub.cpp:652`, `lib/Txt/Txt.cpp:146`.

---

## Image Rendering Pipeline

### PageImage::render — Grayscale Skip

`lib/Epub/Epub/Page.cpp:28-31` — Images must be skipped in GRAYSCALE_LSB/MSB passes:

```cpp
void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images are only rendered in BW mode; grayscale passes are for text anti-aliasing only
  if (renderer.getRenderMode() != GfxRenderer::BW) return;
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}
```

**Why**: Dithered image pixels with values 1 or 2 would otherwise be picked up by the grayscale LUT and given gray waveforms, causing subtle ghosting around image edges. The LUT's `0b00` entry (what all image pixels become when skipped) = "no waveform" = pixel unchanged.

Note: `Page.cpp` must `#include <GfxRenderer.h>` explicitly — `Block.h` only forward-declares `GfxRenderer`, which is insufficient for member access.

### Kindle Dual-Image Pattern

Some Kindle-format EPUBs include two `<img>` tags for the same image — one high-res, one low-res fallback for old Kindle M8 hardware:

```html
<img src="high-res.jpeg" class="high-res" data-AmznRemoved="mobi7"/>
<img src="low-res.jpeg" class="low-res" width="120" height="120" data-AmznRemoved-M8="true"/>
```

Kindle/Calibre hides one via CSS `@media` query. `ChapterHtmlSlimParser` does not evaluate media queries → both render → two stacked images of different sizes.

**Fix** (`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`): During `<img>` attribute scan, skip any image with `data-AmznRemoved-M8` attribute:

```cpp
} else if (strncmp(atts[i], "data-AmznRemoved-M8", 19) == 0) {
  amznM8Removed = true;
}
// After loop:
if (amznM8Removed) {
  LOG_DBG("EHP", "Skipping Kindle M8 low-res fallback image");
  return;
}
```

**Cache note**: After this fix, delete `.crosspoint/epub_<hash>/` for affected books — stale `.pxc` files for the skipped image may linger.

---

## Display / Grayscale Pipeline

### GfxRenderer Pixel Convention

- `drawPixel(x, y, true)` → CLEARS bit → 0 = **black**
- `drawPixel(x, y, false)` → SETS bit → 1 = **white**
- `clearScreen(0xFF)` = all white (default page clear)
- `clearScreen(0x00)` = all black (used before grayscale passes)

### lut_grayscale entries (SSD1677)

Indexed by 2-bit value (RED_bit=MSB, BW_bit=LSB):
- `0b00`: no waveform → pixel **unchanged**
- `0b01`: light gray waveform
- `0b10`: gray waveform
- `0b11`: dark gray waveform

### imagePageWithAA Render Sequence

Triggered when `page->hasImages() && SETTINGS.textAntiAliasing`:

1. BW render (image drawn to framebuffer)
2. `fillRect` blanks image area → `FAST_REFRESH` (shows text only, image area white)
3. Re-render with image → `FAST_REFRESH` (shows text + image)
4. `storeBwBuffer()` → `clearScreen(0x00)` → GRAYSCALE_LSB render → `copyGrayscaleLsbBuffers()`
5. `clearScreen(0x00)` → GRAYSCALE_MSB render → `copyGrayscaleMsbBuffers()`
6. `displayGrayBuffer()` (custom LUT, 61ms) → `restoreBwBuffer()` + `cleanupGrayscaleBuffers()`

### Single Buffer Mode Post-Refresh

After every `displayBuffer(FAST_REFRESH)`, RED RAM is synced with current frameBuffer. After `restoreBwBuffer()`, `cleanupGrayscaleBuffers(frameBuffer)` also syncs RED RAM with the restored BW content — required for correct differential fast refreshes on subsequent page turns.

---

## POSIX TZ Offset Convention

POSIX TZ sign is **inverted** from ISO 8601. `"UTC-1"` = 1 hour EAST (UTC+1).

Formula used in `TimeStore::applyTimezone()`:
```cpp
const int posixOffset = 12 - static_cast<int>(SETTINGS.timezoneIndex);
// timezoneIndex: 0=UTC-12, 12=UTC+0, 26=UTC+14
```

---

## LyraTheme Overrides BaseTheme

`LyraTheme::drawHeader()` overrides `BaseTheme::drawHeader()` and does **not** call super. Any rendering added to `BaseTheme::drawHeader()` will not appear with the Lyra theme unless explicitly duplicated in `LyraTheme::drawHeader()`.
