#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <ShapingTokens.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {
// Raw DEFLATE stored blocks keep the compressed fixtures readable and deterministic.
std::vector<uint8_t> deflate(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> result;
  result.reserve(bytes.size() + 10);
  for (size_t offset = 0; offset < bytes.size();) {
    const auto count = static_cast<uint16_t>(std::min<size_t>(65535, bytes.size() - offset));
    result.push_back(offset + count == bytes.size() ? 1 : 0);
    result.push_back(count & 255);
    result.push_back(count >> 8);
    result.push_back((~count) & 255);
    result.push_back((~count) >> 8);
    result.insert(result.end(), bytes.begin() + offset, bytes.begin() + offset + count);
    offset += count;
  }
  return result;
}

struct GroupFont {
  std::array<EpdGlyph, 3> glyphs{};
  std::array<EpdFontGroup, 3> groups{};
  std::array<uint16_t, 3> mapping{0, 1, 2};
  EpdUnicodeInterval interval{'A', 'C', 0};
  std::vector<uint8_t> bitmap;
  EpdFontData data{};

  static uint8_t pattern(size_t i) { return std::array<uint8_t, 3>{0x1B, 0xE4, 0xB1}[i]; }

  explicit GroupFont(bool frequency, std::array<uint32_t, 3> sizes = {8, 4, 2}) {
    bitmap.reserve(sizes[0] + sizes[1] + sizes[2] + 20);
    for (size_t i = 0; i < glyphs.size(); ++i) {
      // Odd widths require row-aligned -> packed conversion; groups may have padding.
      glyphs[i] = {5, 1, 80, 0, 1, 2, 0};
      const auto compressed = deflate(std::vector<uint8_t>(sizes[i], pattern(i)));
      groups[i] = {static_cast<uint32_t>(bitmap.size()), static_cast<uint32_t>(compressed.size()), sizes[i], 1,
                   static_cast<uint32_t>(i)};
      bitmap.insert(bitmap.end(), compressed.begin(), compressed.end());
    }
    data.bitmap = bitmap.data();
    data.glyph = glyphs.data();
    data.intervals = &interval;
    data.intervalCount = 1;
    data.is2Bit = true;
    data.groups = groups.data();
    data.groupCount = groups.size();
    data.glyphToGroup = frequency ? mapping.data() : nullptr;
  }
};

void expectPackedGlyphs(FontDecompressor& decoder, const GroupFont& font) {
  for (size_t i = 0; i < font.glyphs.size(); ++i) {
    const auto* bitmap = decoder.getBitmap(&font.data, &font.glyphs[i], i);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(bitmap[0], GroupFont::pattern(i));
    EXPECT_EQ(bitmap[1], GroupFont::pattern(i) & 0xC0);  // Five pixels; unused trailing bits remain clear.
  }
}

TEST(FontGroupAdmission, DominantBodyKeepsFourSlotsAcrossBwAndAaWithExplicitFallback) {
  for (const bool psram : {false, true}) {
    fakeheap::reset(psram);
    GroupFont regular(false), bold(true), italic(false), boldItalic(true), heading(false);
    EpdFont r(&regular.data), b(&bold.data), i(&italic.data), bi(&boldItalic.data), h(&heading.data);
    std::map<int, EpdFontFamily> families{{1, EpdFontFamily(&r, &b, &i, &bi)}, {2, EpdFontFamily(&h)}};
    std::map<int, SdCardFont*> sdFonts;
    FontDecompressor decoder;
    FontCacheManager manager(families, sdFonts);
    manager.setFontDecompressor(&decoder);
    std::string body;
    for (unsigned n = 0; n < 40; ++n) body += "ABC";
    auto scope = manager.createPrewarmScope();
    manager.recordText(body.c_str(), 1, EpdFontFamily::REGULAR);
    manager.recordText("ABC", 1, EpdFontFamily::BOLD);
    manager.recordText("ABC", 1, EpdFontFamily::ITALIC);
    manager.recordText("ABC", 1, EpdFontFamily::BOLD_ITALIC);
    manager.recordText("ABC", 2, EpdFontFamily::REGULAR);
    ASSERT_TRUE(scope.endScanAndPrewarm());  // Refusal is intentional, not a missing-pixel failure.
    EXPECT_EQ(decoder.getStats().prewarmSlotRefusals, 1u);
    EXPECT_EQ(decoder.getStats().pageBufferBytes, 24u);  // Four slots, three 2-byte glyphs each.
    EXPECT_EQ(decoder.getStats().pageGlyphsBytes, 144u);
    // Same requests in BW, LSB, MSB. Validate every packed pixel, including the
    // refused fifth group; a single hot group cannot hide alternating A/B/C work.
    for (unsigned plane = 0; plane < 3; ++plane) {
      for (unsigned n = 0; n < 40; ++n) expectPackedGlyphs(decoder, regular);
      expectPackedGlyphs(decoder, bold);
      expectPackedGlyphs(decoder, italic);
      expectPackedGlyphs(decoder, boldItalic);
      expectPackedGlyphs(decoder, heading);
    }
    EXPECT_EQ(decoder.getStats().cacheMisses, 9u);
    EXPECT_EQ(decoder.getStats().cacheHits, 387u);
    decoder.clearCache();
    decoder.resetStats();
    EXPECT_EQ(decoder.getStats().pageBufferBytes, 0u);
  }
}

TEST(FontScratch, ReusesOneAllocationAndReleasesItBeforeReturning) {
  for (bool frequency : {false, true}) {
    fakeheap::reset(false);
    GroupFont font(frequency);
    FontDecompressor decoder;
    ASSERT_EQ(decoder.prewarmCache(&font.data, "ABCABC"), 0);
    EXPECT_EQ(fakeheap::internal.attempts, 1u);
    EXPECT_TRUE(fakeheap::live.empty());
    EXPECT_EQ(decoder.getStats().peakTempBytes, 8u);
    expectPackedGlyphs(decoder, font);
    EXPECT_EQ(decoder.getStats().cacheHits, 3u);
    EXPECT_EQ(decoder.getStats().cacheMisses, 0u);
  }
}

TEST(FontScratch, GrowthNeverNeedsTwoScratchBuffers) {
  fakeheap::reset(false);
  // Total simulated free space can hold the largest buffer, but not old + new.
  fakeheap::internal.free = fakeheap::internal.largest = 8;
  GroupFont font(true, {2, 8, 4});
  FontDecompressor decoder;
  ASSERT_EQ(decoder.prewarmCache(&font.data, "ABC"), 0);
  EXPECT_EQ(fakeheap::internal.attempts, 2u);
  EXPECT_EQ(fakeheap::internal.free, 8u);
  EXPECT_TRUE(fakeheap::live.empty());
  expectPackedGlyphs(decoder, font);
}

TEST(FontScratch, FailedGrowthStillPreparesLaterSmallerGroupsAndAllowsRetry) {
  fakeheap::reset(false);
  fakeheap::internal.failOnAttempt = 2;
  GroupFont font(false, {2, 8, 4});
  FontDecompressor decoder;
  ASSERT_EQ(decoder.prewarmCache(&font.data, "ABC"), 1);
  EXPECT_EQ(fakeheap::internal.attempts, 3u);
  EXPECT_TRUE(fakeheap::live.empty());
  // A and C survived; only B needs the existing on-demand fallback.
  expectPackedGlyphs(decoder, font);
  EXPECT_EQ(decoder.getStats().cacheHits, 2u);
  EXPECT_EQ(decoder.getStats().cacheMisses, 1u);
  decoder.clearCache();
  EXPECT_EQ(decoder.prewarmCache(&font.data, "ABC"), 0);
  EXPECT_TRUE(fakeheap::live.empty());
}

TEST(FontScratch, FailedFirstAllocationStillPreparesSmallerGroups) {
  fakeheap::reset(false);
  fakeheap::internal.largest = 4;
  GroupFont font(true);
  FontDecompressor decoder;
  ASSERT_EQ(decoder.prewarmCache(&font.data, "ABC"), 1);
  EXPECT_EQ(fakeheap::internal.attempts, 2u);
  EXPECT_TRUE(fakeheap::live.empty());
  expectPackedGlyphs(decoder, font);
  EXPECT_EQ(decoder.getStats().cacheHits, 2u);
}

TEST(FontScratch, CorruptGroupDoesNotPoisonReusedScratch) {
  fakeheap::reset(false);
  GroupFont font(true);
  font.bitmap[font.groups[1].compressedOffset] = 7;  // Reserved DEFLATE block type.
  FontDecompressor decoder;
  ASSERT_EQ(decoder.prewarmCache(&font.data, "ABC"), 1);
  EXPECT_EQ(fakeheap::internal.attempts, 1u);
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_NE(decoder.getBitmap(&font.data, &font.glyphs[0], 0), nullptr);
  EXPECT_NE(decoder.getBitmap(&font.data, &font.glyphs[2], 2), nullptr);
  EXPECT_EQ(decoder.getBitmap(&font.data, &font.glyphs[1], 1), nullptr);
}

TEST(FontScratch, HandlesLargestGeneratedGroupAndZeroSizeGlyph) {
  fakeheap::reset(false);
  GroupFont font(false, {65536, 4, 2});
  font.glyphs[2].width = font.glyphs[2].height = font.glyphs[2].dataLength = 0;
  FontDecompressor decoder;
  ASSERT_EQ(decoder.prewarmCache(&font.data, "ABC"), 0);
  EXPECT_EQ(fakeheap::internal.attempts, 1u);
  EXPECT_EQ(decoder.getStats().peakTempBytes, 65536u);
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_NE(decoder.getBitmap(&font.data, &font.glyphs[2], 2), nullptr);
  EXPECT_EQ(decoder.getStats().cacheHits, 1u);
}

TEST(VerticalGlyphBounds, ConsumesOneLookupAndKeepsFallbackMetrics) {
  struct Lookups {
    int calls = 0;
    EpdGlyph body = {5, 7, 80, 0, 6, 0, 0};
    EpdGlyph quote = {2, 3, 40, 0, 9, 0, 0};
    EpdGlyph replacement = {4, 5, 64, 0, 5, 0, 0};
  } lookups;
  EpdFontData data{};
  data.ascender = 10;
  data.glyphMissCtx = &lookups;
  data.glyphMissHandler = [](void* context, uint32_t cp) -> const EpdGlyph* {
    auto& counts = *static_cast<Lookups*>(context);
    ++counts.calls;
    switch (cp) {
      case 'v':
        return &counts.body;
      case 0x2018:
      case shaping::glyphToken(1):
        return &counts.quote;
      case 0xfffd:
        return &counts.replacement;
      default:
        return nullptr;
    }
  };
  EpdFont font(&data);
  HalDisplay display(80, 80);
  GfxRenderer renderer(display);
  renderer.insertFont(1, EpdFontFamily(&font));

  auto bounds = renderer.getTextVerticalBounds(1, "vvvv");
  EXPECT_EQ(lookups.calls, 1);  // All four letters share the first measurement.
  EXPECT_EQ(bounds.top, 4);
  EXPECT_EQ(bounds.bottom, 11);

  // Measure a stored display form with both a cached letter and a positioned glyph.
  std::string shaped = "v";
  utf8AppendCodepoint(shaping::offsetToken(0, -3), shaped);
  utf8AppendCodepoint(shaping::glyphToken(1), shaped);
  lookups.calls = 0;
  bounds =
      renderer.getTextVerticalBounds(1, "vv", EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, 0, shaped.c_str());
  EXPECT_EQ(lookups.calls, 1);
  EXPECT_EQ(bounds.top, -2);
  EXPECT_EQ(bounds.bottom, 11);

  lookups.calls = 0;
  bounds = renderer.getTextVerticalBounds(1, "\xca\xbb");  // Alias turned comma to left quotation mark.
  EXPECT_EQ(lookups.calls, 2);
  EXPECT_EQ(bounds.top, 1);
  EXPECT_EQ(bounds.bottom, 4);

  lookups.calls = 0;
  bounds = renderer.getTextVerticalBounds(1, "x");
  EXPECT_EQ(lookups.calls, 2);
  EXPECT_EQ(bounds.top, 5);
  EXPECT_EQ(bounds.bottom, 10);

  lookups.calls = 0;
  bounds = renderer.getTextVerticalBounds(1, "\xc2\xa0");  // Blank nonbreaking space has no vertical ink.
  EXPECT_EQ(lookups.calls, 1);
  EXPECT_EQ(bounds.top, bounds.bottom);
}

TEST(ScaledFallback, MatchesDrawingFromTheOwningFont) {
  fakeheap::reset(false);
  // Include the runtime TTF bitmap callback in the scalable-enabled test target.
#if CROSSINK_SCALABLE_FONTS
  constexpr int sourceCount = 3;
#else
  constexpr int sourceCount = 2;
#endif
  for (int source = 0; source < sourceCount; ++source) {
    const bool compressed = source == 1;
    // Primary A is blank; fallback Z has ink. Identical metrics isolate ownership.
    EpdGlyph glyph{5, 3, 80, 0, 3, 4, 0};
    EpdUnicodeInterval primaryInterval{'A', 'A', 0}, fallbackInterval{'Z', 'Z', 0};
    const uint8_t blank[] = {0, 0, 0, 0};
    const uint8_t packed[] = {0xFF, 0xFF, 0xFF, 0xFC};
    const auto encoded = deflate({0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0});
    EpdFontGroup group{0, static_cast<uint32_t>(encoded.size()), 6, 1, 0};
    EpdFontData primaryData{};
    primaryData.bitmap = blank;
    primaryData.glyph = &glyph;
    primaryData.intervals = &primaryInterval;
    primaryData.intervalCount = 1;
    primaryData.advanceY = 8;
    primaryData.ascender = 3;
    primaryData.is2Bit = true;
    EpdFontData fallbackData = primaryData;
    EpdGlyph fallbackGlyph = glyph;  // Distinct glyph array, as on real devices.
    fallbackData.glyph = &fallbackGlyph;
    fallbackData.intervals = &fallbackInterval;
    fallbackData.bitmap = compressed ? encoded.data() : packed;
    fallbackData.groups = compressed ? &group : nullptr;
    fallbackData.groupCount = compressed ? 1 : 0;
#if CROSSINK_SCALABLE_FONTS
    if (source == 2) {
      fallbackData.bitmap = nullptr;
      fallbackData.glyphMissCtx = const_cast<uint8_t*>(packed);
      fallbackData.bitmapHandler = [](void* context, const EpdGlyph*) { return static_cast<const uint8_t*>(context); };
    }
#endif
    EpdFont primary(&primaryData), fallback(&fallbackData);
    HalDisplay display(80, 80);
    GfxRenderer renderer(display);
    renderer.begin();
    FontDecompressor decoder;
    FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
    cache.setFontDecompressor(&decoder);
    renderer.setFontCacheManager(&cache);
    renderer.insertFont(1, EpdFontFamily(&primary, nullptr, nullptr, nullptr, &fallback));
    renderer.insertFont(2, EpdFontFamily(&fallback));
    // Styled face -> regular fallback must also keep the selected face's bitmap.
    renderer.insertFont(3, EpdFontFamily(&fallback, &primary));
    for (auto style : {EpdFontFamily::SUP, EpdFontFamily::SUB, EpdFontFamily::SMALL_CAPS}) {
      for (int orientation = 0; orientation < 4; ++orientation) {
        renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
        const char* text = style == EpdFontFamily::SMALL_CAPS ? "z" : "Z";
        renderer.clearScreen();
        const auto blankFrame = display.bw;
        renderer.drawText(2, 20, 20, text, true, style);
        const auto expected = display.bw;
        ASSERT_NE(expected, blankFrame);
        for (int fontId : {1, 3}) {
          renderer.clearScreen();
          renderer.drawText(fontId, 20, 20, text, true, static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD));
          EXPECT_EQ(display.bw, expected) << source << ' ' << int(style) << ' ' << orientation << ' ' << fontId;
        }
      }
    }
  }
}
}  // namespace
