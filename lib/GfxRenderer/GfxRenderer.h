#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;

#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../ScreenCalibration/ScreenInsets.h"
#include "Bitmap.h"

namespace glyphBitmap {
struct Frame;
}

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
#ifdef SIMULATOR
  friend struct UpstreamFeatureSmokeTest;
#endif
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  ScreenInsets viewableInsets;
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  mutable bool absoluteGrayPlanes = false;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint32_t frameBufferLoans = 0;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  void* bookFontContext_ = nullptr;
  int (*bookFontResolver_)(void*, uint32_t, uint8_t) = nullptr;
  uint32_t (*bookFontUsage_)(void*) = nullptr;
  uint32_t (*bookFontLoaded_)(void*) = nullptr;
  uint32_t (*bookFontSignature_)(void*, uint32_t) = nullptr;
  bool (*bookFontRestore_)(void*, uint32_t, uint32_t, uint32_t) = nullptr;
  // Shared bitmap row buffers. Every read/write must be inside BitmapScratchLock;
  // ensureBitmapScratchBuffers() asserts that contract before exposing them.
  mutable SemaphoreHandle_t bitmapScratchMutex_ = nullptr;
  mutable uint8_t* bitmapScratchOutputRow_ = nullptr;
  mutable size_t bitmapScratchOutputRowSize_ = 0;
  mutable uint8_t* bitmapScratchRowBytes_ = nullptr;
  mutable size_t bitmapScratchRowBytesSize_ = 0;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) instead of the shared framebuffer.
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;

  // Optional logical-space clip used while a TextBlock is rendered inside a
  // table cell. Glyph bitmaps and background fills both pass through this
  // guard, so an emergency oversized codepoint cannot paint into a neighbour.
  mutable bool textClipActive_ = false;
  mutable int textClipLeft_ = 0;
  mutable int textClipTop_ = 0;
  mutable int textClipRight_ = 0;   // half-open
  mutable int textClipBottom_ = 0;  // half-open

  class BitmapScratchLock {
    const GfxRenderer& renderer_;
    bool locked_ = false;

   public:
    explicit BitmapScratchLock(const GfxRenderer& renderer);
    BitmapScratchLock(const BitmapScratchLock&) = delete;
    BitmapScratchLock& operator=(const BitmapScratchLock&) = delete;
    ~BitmapScratchLock();

    bool isLocked() const { return locked_; }
  };

  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Explicit filename-only font ids, with built-in glyphs as the primary face.
  std::map<int, int> filenameFontMap_;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  void freeBitmapScratchBuffers();
  bool ensureBitmapScratchBuffers(size_t outputRowSize, size_t rowBytesSize) const;
  bool bitmapScratchLockHeldByCurrentTask() const;
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay),
        renderMode(BW),
        orientation(Portrait),
        fadingFix(false),
        bitmapScratchMutex_(xSemaphoreCreateMutex()) {
    assert(bitmapScratchMutex_ != nullptr && "Failed to create GfxRenderer bitmap scratch mutex");
  }
  GfxRenderer(const GfxRenderer&) = delete;
  GfxRenderer& operator=(const GfxRenderer&) = delete;
  GfxRenderer(GfxRenderer&&) = delete;
  GfxRenderer& operator=(GfxRenderer&&) = delete;
  ~GfxRenderer() {
    freeBwBufferChunks();
    freeBitmapScratchBuffers();
  }

  static constexpr int VIEWABLE_MARGIN_TOP = ScreenInsets{}.edges[0];
  static constexpr int VIEWABLE_MARGIN_RIGHT = ScreenInsets{}.edges[1];
  static constexpr int VIEWABLE_MARGIN_BOTTOM = ScreenInsets{}.edges[2];
  static constexpr int VIEWABLE_MARGIN_LEFT = ScreenInsets{}.edges[3];

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Shared UI families fill missing characters; legacy metadata aliases use
  // the same composites. Clear all borrowed pointers before releasing faces.
  int filenameFontId(int primaryFontId) const;
  bool setFilenameFallback(int primaryFontId, int compositeFontId, const EpdFont* regular, const EpdFont* bold);
  void clearFilenameFallbacks();
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const uint32_t* codepoints, uint32_t cpCount, bool includeSpace,
                             bool includeHyphen, uint8_t styleMask = 0x0F) const;
  // Adds shaped RTL presentation forms to one bounded style bucket. The caller
  // owns the reusable scratch strings so clipping can batch pages without a
  // temporary heap allocation for every word.
  bool collectSdCardFontShapedRtlCodepoints(const char* utf8Text, uint32_t* codepoints, uint16_t& cpCount,
                                            uint16_t capacity, std::string& tokenScratch,
                                            std::string& visualScratch) const;
  bool releaseSdCardFontForLowMemory(int fontId, bool preserveAdvanceTable = false) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;
  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  bool supportsAsyncGrayscaleBase() const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void invertRect(int x, int y, int width, int height) const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;
  const ScreenInsets& getViewableInsets() const { return viewableInsets; }
  void setViewableInsets(const ScreenInsets& insets) {
    if (insets.valid()) viewableInsets = insets;
  }

  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;
  bool isStripTargetActive() const { return _stripActive; }
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  bool isPixelBlack(int x, int y) const;
  void drawPixel(int x, int y, bool state = true) const;
  // Unscaled glyphs share one clipped, orientation-aware rasterizer.
  void drawGlyphBitmap(const uint8_t* bitmap, int width, int height, const glyphBitmap::Frame& frame, bool twoBit,
                       RenderMode mode, bool state) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawImageInverted(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int size) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int width, int height) const;
  // monochromeMask draws nonwhite pixels as black without changing the active
  // grayscale session; white pixels remain transparent.
  bool drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  bool monochromeMask = false) const;
  bool drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  // Counter-invert content images in the logical framebuffer so output-level
  // Dark Mode leaves their original polarity unchanged.
  void preserveImagePolarity(int x, int y, int width, int height) const;
  bool isDisplayInverted() const { return display.isInverted(); }
  // Trapezoidal blit used by Flow/iPod-style carousels. Fits the bitmap into a
  // bounding box of width `w` and height `max(hL, hR)` whose top-left is (x, y).
  void drawPerspectiveBitmap(const Bitmap& bitmap, int x, int y, int w, int hL, int hR) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;
  // Snapshot / restore a screen-coordinate framebuffer region. The renderer
  // byte-aligns the panel-memory rectangle internally, so callers must pass the
  // same rectangle to restore the saved pixels.
  size_t readFramebufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* dst, size_t dstCapacity) const;
  void writeFramebufferRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* src);

  // Text
  struct TextVerticalBounds {
    int top = 0;
    int bottom = 0;
  };
  // Visible regular-text glyph bounds relative to the y coordinate passed to drawText().
  TextVerticalBounds getTextVerticalBounds(int fontId, const char* text) const;
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t characterSpacing = 0) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t characterSpacing = 0) const;
  // Guard text/background pixels while a table cell is rendered. The guard is
  // intentionally single-level and scoped by the caller; nested use is a
  // programming error caught in debug builds.
  void beginTextClip(int x, int y, int width, int height) const;
  void endTextClip() const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style,
                 int8_t characterSpacing = 0) const;
  /// Returns the rendered advance of \p text. When \p followingCp is supplied,
  /// includes its kerning with the final glyph in the same fixed-point rounding
  /// step that drawText() uses, without drawing or consuming that codepoint.
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, uint32_t followingCp = 0,
                      int8_t characterSpacing = 0) const;
  bool getCodepointMetrics(int fontId, uint32_t cp, EpdFontFamily::Style style, int32_t& advanceFP, int& top) const;
  int drawScaledCodepoint(int fontId, uint32_t cp, EpdFontFamily::Style style, int x, int baselineY, int scale256,
                          bool pixelState = true) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  // Zero means bitmap font: EPUB content sizing stays disabled.
  uint8_t getFontPointSize(int fontId) const;
  int getFontIdForSize(int fontId, uint8_t points) const;
  void setBookFontResolver(void* context, int (*resolve)(void*, uint32_t, uint8_t), uint32_t (*usage)(void*) = nullptr,
                           uint32_t (*loaded)(void*) = nullptr, uint32_t (*signature)(void*, uint32_t) = nullptr,
                           bool (*restore)(void*, uint32_t, uint32_t, uint32_t) = nullptr) {
    bookFontContext_ = context;
    bookFontResolver_ = resolve;
    bookFontUsage_ = usage;
    bookFontLoaded_ = loaded;
    bookFontSignature_ = signature;
    bookFontRestore_ = restore;
  }
  uint32_t bookFontUsage() const { return bookFontUsage_ ? bookFontUsage_(bookFontContext_) : 0; }
  uint32_t bookFontLoaded() const { return bookFontLoaded_ ? bookFontLoaded_(bookFontContext_) : 0; }
  uint32_t bookFontSignature(uint32_t mask) const {
    return bookFontSignature_ ? bookFontSignature_(bookFontContext_, mask) : 0;
  }
  bool restoreBookFonts(uint32_t mask, uint32_t loaded, uint32_t signature) const {
    return bookFontRestore_ ? bookFontRestore_(bookFontContext_, mask, loaded, signature)
                            : mask == 0 && loaded == 0 && signature == 0;
  }
  int resolveBookFont(int readerFontId, uint32_t family, uint8_t points = 0) const {
    if (!points) points = getFontPointSize(readerFontId);
    const int id = family && bookFontResolver_ ? bookFontResolver_(bookFontContext_, family, points) : 0;
    return id ? id : getFontIdForSize(readerFontId, points);
  }
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(RenderMode mode);
  bool grayPlanesAreAbsolute() const { return absoluteGrayPlanes; }
  bool supportsAbsoluteGrayscale() const;
  bool displayAbsoluteGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  // Direct grayscale folds the B/W base into the grayscale pass
  // (GrayscaleBase::Combined) instead of pushing a separate base refresh first.
  // It encodes complete planes just like Absolute, and every panel driver that
  // reports Direct also reports Absolute, so Direct is only ever an upgrade.
  // The GrayscaleMode enum stays out of this header: the simulator's HalDisplay
  // does not define it.
  bool supportsDirectGrayscale() const;
  bool displayDirectGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH,
                            bool turnOffScreen = false) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(bool turnOffScreen = false) const;
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool shouldSkipImageBlanking() const;
  bool supportsStripGrayscale() const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }
  // RenderLock protects the counter. Every loan returns blank storage, so
  // incremental callers compare it to detect destruction of the visible page.
  uint32_t frameBufferLoanCount() const { return frameBufferLoans; }

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
