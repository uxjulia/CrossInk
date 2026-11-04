#include <EpdFontFamily.h>
#include <Utf8.h>
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/ui_symbols_10.h>
#include <gtest/gtest.h>

namespace {
constexpr uint32_t POWER = 0x23FB;
const EpdFont symbols(&ui_symbols_10);
const EpdFont smallRegular(&inter_10_regular), smallBold(&inter_10_bold);
const EpdFont largeRegular(&inter_12_regular), largeBold(&inter_12_bold);
const EpdFontFamily small(&smallRegular, &smallBold, nullptr, nullptr, &symbols);
const EpdFontFamily large(&largeRegular, &largeBold, nullptr, nullptr, &symbols);
}  // namespace

TEST(UiSymbolFallback, ContainsExactlyOneGlyph) {
  EXPECT_EQ(sizeof(ui_symbols_10Glyphs) / sizeof(ui_symbols_10Glyphs[0]), 1u);
  EXPECT_EQ(sizeof(ui_symbols_10Intervals) / sizeof(ui_symbols_10Intervals[0]), 1u);
  EXPECT_EQ(ui_symbols_10Intervals[0].first, POWER);
  EXPECT_TRUE(symbols.hasCodepoint(POWER));
  EXPECT_FALSE(symbols.hasCodepoint('A'));
}

TEST(UiSymbolFallback, SharesTheSameRasterAtBothScalesAndStyles) {
  for (const auto* family : {&small, &large}) {
    for (const auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
      const auto glyph = family->getGlyphData(POWER, style);
      EXPECT_EQ(glyph.fontData, &ui_symbols_10);
      EXPECT_EQ(glyph.glyph, symbols.findGlyph(POWER));
      EXPECT_TRUE(family->hasCodepoint(POWER, style));
      EXPECT_EQ(family->getFallbackCodepoint(POWER, style), POWER);
      int width = 0, height = 0;
      family->getTextDimensions("⏻", &width, &height, style);
      EXPECT_EQ(width, 18);
      EXPECT_EQ(height, 18);
    }
  }
}

TEST(UiSymbolFallback, PreservesNormalGlyphsAndMissingGlyphBehavior) {
  EXPECT_EQ(small.getGlyphData('A').fontData, &inter_10_regular);
  EXPECT_EQ(large.getGlyphData('*', EpdFontFamily::BOLD).fontData, &inter_12_bold);
  const EpdFontFamily noFallback(&smallRegular);
  EXPECT_FALSE(noFallback.hasCodepoint(POWER));
  EXPECT_EQ(noFallback.getGlyphData(POWER).glyph, smallRegular.getGlyph(REPLACEMENT_GLYPH));
}

namespace {
constexpr EpdUnicodeInterval cjkIntervals[] = {{0x4E00, 0x4E01, 0}};
constexpr EpdGlyph cjkGlyphs[] = {{18, 18, 20 * 16, 0, 16, 81, 0}, {18, 18, 20 * 16, 0, 16, 81, 0}};
constexpr EpdGlyph cjkBoldGlyphs[] = {{19, 18, 21 * 16, 0, 16, 86, 0}};
constexpr EpdUnicodeInterval cjkBoldIntervals[] = {{0x4E00, 0x4E00, 0}};
EpdFontData cjkData(const EpdGlyph* glyphs, const EpdUnicodeInterval* intervals) {
  EpdFontData data{};
  data.glyph = glyphs;
  data.intervals = intervals;
  data.intervalCount = 1;
  data.advanceY = 24;
  data.ascender = 18;
  data.descender = -4;
  return data;
}
const auto cjkRegularData = cjkData(cjkGlyphs, cjkIntervals);
const auto cjkBoldData = cjkData(cjkBoldGlyphs, cjkBoldIntervals);
const EpdFont cjkRegular(&cjkRegularData), cjkBold(&cjkBoldData);
}  // namespace

TEST(FilenameFallback, PreservesLatinAndSharedSymbolsWithinMixedText) {
  const auto composite = small.withFallbackFonts(&cjkRegular, &cjkBold);
  EXPECT_EQ(composite.getGlyphData('A').fontData, &inter_10_regular);
  EXPECT_EQ(composite.getGlyphData('A', EpdFontFamily::BOLD).fontData, &inter_10_bold);
  EXPECT_EQ(composite.getGlyphData(POWER, EpdFontFamily::BOLD).fontData, &ui_symbols_10);
  EXPECT_EQ(composite.getGlyphData(0x4E00).fontData, &cjkRegularData);
  EXPECT_EQ(composite.getGlyphData(0x4E00, EpdFontFamily::BOLD).fontData, &cjkBoldData);
  EXPECT_EQ(composite.getData()->ascender, small.getData()->ascender);
  int primaryW = 0, primaryH = 0, compositeW = 0, compositeH = 0;
  small.getTextDimensions("Latin Volume 2", &primaryW, &primaryH);
  composite.getTextDimensions("Latin Volume 2", &compositeW, &compositeH);
  EXPECT_EQ(primaryW, compositeW);
  EXPECT_EQ(primaryH, compositeH);
}

TEST(FilenameFallback, UsesRegularWhenBoldIsMissingOrLacksAGlyph) {
  const auto withoutBold = small.withFallbackFonts(&cjkRegular);
  EXPECT_EQ(withoutBold.getGlyphData(0x4E00, EpdFontFamily::BOLD).fontData, &cjkRegularData);
  const auto partialBold = small.withFallbackFonts(&cjkRegular, &cjkBold);
  EXPECT_EQ(partialBold.getGlyphData(0x4E01, EpdFontFamily::BOLD).fontData, &cjkRegularData);
  EXPECT_TRUE(partialBold.hasCodepoint(0x4E01, EpdFontFamily::BOLD));
}

TEST(UiSymbolFallback, RemovingBorrowedFacesPreservesBuiltinsAndSharedSymbols) {
  const auto restored = small.withFallbackFonts(&cjkRegular, &cjkBold).withFallbackFonts(nullptr);
  for (const auto style :
       {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
    EXPECT_FALSE(restored.hasCodepoint(0x4E00, style));
    EXPECT_EQ(restored.getGlyphData('A', style).glyph, small.getGlyphData('A', style).glyph);
    EXPECT_EQ(restored.getGlyphData(POWER, style).fontData, &ui_symbols_10);
  }
}

TEST(FilenameFallback, MeasuresFallbackGlyphsAndPreservesMissingGlyphBehavior) {
  const auto composite = small.withFallbackFonts(&cjkRegular);
  int width = 0, height = 0;
  composite.getTextDimensions("一丁", &width, &height);
  EXPECT_EQ(width, 38);  // 20-pixel advance plus the last glyph's 18-pixel ink
  EXPECT_EQ(height, 18);
  EXPECT_EQ(composite.getFallbackCodepoint(0x4E00), 0x4E00u);
  EXPECT_EQ(composite.getGlyphData(0x4E02).glyph, smallRegular.getGlyph(REPLACEMENT_GLYPH));
  EXPECT_EQ(composite.getKerning('A', 0x4E00), 0);
  EXPECT_EQ(composite.getKerning(0x4E00, 'V'), 0);
}

TEST(FilenameFallback, SupportsNonCjkGlyphsWithoutAProbeList) {
  constexpr EpdUnicodeInterval armenianIntervals[] = {{0x0531, 0x0532, 0}};
  const auto data = cjkData(cjkGlyphs, armenianIntervals);
  const EpdFont armenian(&data);
  const auto composite = small.withFallbackFonts(&armenian);
  EXPECT_EQ(composite.getGlyphData(0x0531).fontData, &data);
  EXPECT_EQ(composite.getGlyphData(0x0532, EpdFontFamily::BOLD).fontData, &data);
  EXPECT_EQ(composite.getGlyphData('A').fontData, &inter_10_regular);
  EXPECT_FALSE(composite.hasCodepoint(0x4E00));
  int width = 0, height = 0;
  composite.getTextDimensions("ԱԲ", &width, &height);
  EXPECT_EQ(width, 38);
}

namespace {
// A small face with no Unicode spaces, and a visible replacement box.
constexpr EpdUnicodeInterval spacingIntervals[] = {{0x20, 0x20, 0}, {'.', '.', 1}, {'0', '0', 2},
                                                   {'A', 'A', 3},   {'M', 'M', 4}, {0xFFFD, 0xFFFD, 5}};
constexpr EpdGlyph spacingGlyphs[] = {{0, 0, 80, 0, 0, 0, 0},     {2, 2, 48, 0, 2, 0, 0},
                                      {8, 12, 128, 0, 12, 0, 0},  {9, 12, 160, 0, 12, 0, 0},
                                      {16, 12, 320, 0, 12, 0, 0}, {10, 12, 192, 0, 12, 0, 0}};
EpdFontData spacingData() {
  EpdFontData data{};
  data.glyph = spacingGlyphs;
  data.intervals = spacingIntervals;
  data.intervalCount = sizeof(spacingIntervals) / sizeof(spacingIntervals[0]);
  data.ascender = 12;
  return data;
}
const auto spacingFontData = spacingData();
const EpdFont spacingFont(&spacingFontData);
const EpdFontFamily spacingFamily(&spacingFont);
}  // namespace

TEST(UnicodeSpaces, MissingSpacesHaveDistinctWidthsWithoutReplacementInk) {
  struct Sample {
    uint32_t cp;
    uint16_t advance;
  };
  const Sample samples[] = {{0x00A0, 80},  {0x1680, 80},  {0x2000, 160}, {0x2001, 320}, {0x2002, 160},
                            {0x2003, 320}, {0x2004, 107}, {0x2005, 80},  {0x2006, 53},  {0x2007, 128},
                            {0x2008, 48},  {0x2009, 64},  {0x200A, 32},  {0x202F, 64},  {0x205F, 71},
                            {0x3000, 320}, {0x0085, 80},  {0x2028, 80},  {0x2029, 80}};
  for (const auto sample : samples) {
    SCOPED_TRACE(sample.cp);
    EXPECT_EQ(spacingFamily.getFallbackCodepoint(sample.cp), sample.cp);
    EXPECT_EQ(spacingFamily.getSpaceAdvanceX(sample.cp), sample.advance);
    char text[12] = "A";
    char* end = utf8AppendCodepoint(text + 1, sample.cp);
    *end++ = 'A';
    *end = 0;
    int width = 0, height = 0;
    spacingFamily.getTextDimensions(text, &width, &height);
    EXPECT_EQ(width, 10 + fp4::toPixel(sample.advance) + 9);
    EXPECT_EQ(height, 12);
    spacingFamily.getTextDimensions(text + 1, &width, &height);
    EXPECT_EQ(width, fp4::toPixel(sample.advance) + 9);
  }
}

TEST(UnicodeSpaces, ControlsStayZeroWidthAndUnrelatedMissingCharactersStillUseABox) {
  for (uint32_t cp :
       {0x180Eu, 0x200Bu, 0x200Cu, 0x200Du, 0x200Eu, 0x200Fu, 0x2060u, 0x2061u, 0x2062u, 0x2063u, 0x2064u, 0xFEFFu}) {
    EXPECT_EQ(spacingFamily.getFallbackCodepoint(cp), cp);
    EXPECT_EQ(spacingFamily.getSpaceAdvanceX(cp), 0);
  }
  for (uint32_t cp = 9; cp <= 13; ++cp) EXPECT_EQ(spacingFamily.getSpaceAdvanceX(cp), 80);
  EXPECT_EQ(spacingFamily.getFallbackCodepoint(0x2603), REPLACEMENT_GLYPH);
  EXPECT_EQ(spacingFamily.getGlyphData(0x2603).glyph, &spacingGlyphs[5]);
}

TEST(UnicodeSpaces, PreservesNativeBlankWidthsAndScalesWithFontStyle) {
  for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
    const auto native = small.findGlyphData(0x2009, style).glyph;
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(small.getSpaceAdvanceX(0x2009, style), native->advanceX);
  }
  EXPECT_LT(spacingFamily.getSpaceAdvanceX(0x200A), spacingFamily.getSpaceAdvanceX(0x2009));
  EXPECT_LT(spacingFamily.getSpaceAdvanceX(0x2009), spacingFamily.getSpaceAdvanceX(0x2002));
  EXPECT_LT(spacingFamily.getSpaceAdvanceX(0x2002), spacingFamily.getSpaceAdvanceX(0x2003));
}

TEST(UnicodeSpaces, VisiblePlaceholderInSpaceSlotStillMeasuresAsBlankSpacing) {
  constexpr EpdUnicodeInterval intervals[] = {{0x2009, 0x2009, 0}};
  constexpr EpdGlyph glyphs[] = {{8, 12, 192, 0, 12, 0, 0}};
  auto data = spacingData();
  data.intervals = intervals;
  data.glyph = glyphs;
  data.intervalCount = 1;
  const EpdFont face(&data);
  const EpdFontFamily family(&face);
  int width = 0, height = 0;
  family.getTextDimensions("\xE2\x80\x89", &width, &height);
  EXPECT_EQ(width, 0);
  EXPECT_EQ(height, 0);
  EXPECT_GT(family.getSpaceAdvanceX(0x2009), 0);
}

TEST(UnicodeSpaces, PreservesNativeOghamStemlineInkAndWidth) {
  constexpr EpdUnicodeInterval intervals[] = {{0x1680, 0x1680, 0}};
  constexpr EpdGlyph glyphs[] = {{8, 2, 160, 0, 2, 0, 0}};
  auto data = spacingData();
  data.intervals = intervals;
  data.glyph = glyphs;
  data.intervalCount = 1;
  const EpdFont face(&data);
  const EpdFontFamily family(&face);
  EXPECT_FALSE(syntheticGlyph::isBlankSpace(0x1680, true));
  EXPECT_TRUE(syntheticGlyph::isBlankSpace(0x1680, false));
  EXPECT_EQ(family.getSpaceAdvanceX(0x1680), 160);
  int width = 0, height = 0;
  family.getTextDimensions("\xE1\x9A\x80", &width, &height);
  EXPECT_EQ(width, 8);
  EXPECT_EQ(height, 2);
}
