#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <ReaderRefreshScope.h>
#include <SdCardFont.h>
#include <TouchReaderPreviewModel.h>
#include <gtest/gtest.h>

#include <array>

#include "Epub/converters/DirectPixelWriter.h"
#include "lib/Epub/Epub/blocks/ImageBlock.cpp"

bool ImageDecoderFactory::supportsMemorySource(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }

namespace {
// Deterministic 2-bit glyphs with negative bearings and descenders. Both the
// built-in and real .cpfont loaders use these bytes, including RTL/CJK/marks.
struct RasterFont {
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> pixels;
  EpdFontData data{};
  EpdFont font{&data};
  explicit RasterFont(int size) {
    for (auto [first, last] : {std::pair{32u, 127u},
                               {0xB7u, 0xB7u},
                               {0x300u, 0x36Fu},
                               {0x590u, 0x6FFu},
                               {0x4E00u, 0x4E20u},
                               {0xFB50u, 0xFEFFu},
                               {0xFFFDu, 0xFFFDu}}) {
      intervals.push_back({first, last, static_cast<uint32_t>(glyphs.size())});
      for (unsigned cp = first; cp <= last; ++cp) {
        EpdGlyph g{};
        g.width = size;
        g.height = size + 4;
        g.advanceX = size * 16;
        g.left = -3;
        g.top = size + 1;
        if (cp >= 0x300 && cp <= 0x36f) {
          g.advanceX = 0;
          g.top = size + 5;
        }
        g.dataOffset = pixels.size();
        g.dataLength = (g.width * g.height + 3) / 4;
        glyphs.push_back(g);
        for (int b = 0; b < g.dataLength; ++b) pixels.push_back(b % 2 ? 0x1B : 0xE4);
      }
    }
    data.bitmap = pixels.data();
    data.glyph = glyphs.data();
    data.intervals = intervals.data();
    data.intervalCount = intervals.size();
    data.advanceY = size + 6;
    data.ascender = size + 1;
    data.descender = -3;
    data.is2Bit = true;
  }
  template <class T>
  static void append(std::vector<uint8_t>& bytes, const T& value) {
    auto p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> bytes(64);
    memcpy(bytes.data(), "CPFONT\0\0", 8);
    auto put16 = [&](int o, uint16_t v) { memcpy(bytes.data() + o, &v, 2); };
    auto put32 = [&](int o, uint32_t v) { memcpy(bytes.data() + o, &v, 4); };
    put16(8, CPFONT_VERSION);
    put16(10, 1);
    bytes[12] = 1;
    put32(36, intervals.size());
    put32(40, glyphs.size());
    bytes[44] = data.advanceY;
    put16(45, data.ascender);
    put16(47, uint16_t(data.descender));
    put32(56, 64);
    for (auto& i : intervals) append(bytes, i);
    for (auto& g : glyphs) append(bytes, g);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    return bytes;
  }
};

TEST(EpubTextGrayscaleTest, CharacterSpacingMeasuresAndDrawsTheSameGlyphPositions) {
  for (bool sd : {false, true}) {
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    HalDisplay display(800, 480);
    GfxRenderer renderer(display);
    renderer.begin();
    SdCardFont sdFont;
    if (sd) {
      Storage.put("spacing.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("spacing.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
      sdFont.prewarm(
          "ABCD A B A\xcc\x81"
          "BCD",
          0x01);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    for (int spacing = -5; spacing <= 5; ++spacing) {
      SCOPED_TRACE(spacing);
      // Four equal glyphs: three adjustable gaps, with half-pixel rounding.
      const int shift = (3 * spacing + 1) >> 1;
      EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR, 0, spacing), 48 + shift);
      EXPECT_EQ(renderer.getTextAdvanceX(1, "A B", EpdFontFamily::REGULAR, 0, spacing), 36);
      EXPECT_EQ(renderer.getTextAdvanceX(1,
                                         "A\xcc\x81"
                                         "BCD",
                                         EpdFontFamily::REGULAR, 0, spacing),
                48 + shift);
      renderer.clearScreen();
      renderer.drawText(1, 20, 20, "ABCD", true, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, spacing);
      const auto actual = display.bw;
      renderer.clearScreen();
      for (int i = 0; i < 4; ++i) {
        char letter[] = {static_cast<char>('A' + i), 0};
        renderer.drawText(1, 20 + i * 12 + ((i * spacing + 1) >> 1), 20, letter);
      }
      EXPECT_EQ(display.bw, actual);
    }
    EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR), 48);
  }
}

TEST(EpubTextGrayscaleTest, RedactionUsesWordAdvanceAndPaintsNoLetters) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  const auto black = TextBlock::WORD_FLAG_BACKGROUND_BLACK;
  TextBlock line(
      {"AB", "C", "D"}, {0, 32, 90}, {EpdFontFamily::UNDERLINE, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {},
      {}, {black, black, 0}, {}, {}, {},
      {TextBlock::packWordSize(0, true), TextBlock::packWordSize(0, true, true), TextBlock::packWordSize(0, true)});
  ASSERT_TRUE(line.valid());
  renderer.clearScreen();
  line.render(renderer, 1, 20, 20);
  const auto actual = display.bw;
  renderer.clearScreen();
  renderer.fillRect(20, 20, 32 + renderer.getTextAdvanceX(1, "C", EpdFontFamily::REGULAR), renderer.getLineHeight(1),
                    true);
  EXPECT_EQ(actual, display.bw);
}

TEST(EpubTextGrayscaleTest, BlackBackgroundCoversFocusedGlyphsAndDescenders) {
  fakeheap::reset(true);
  RasterFont regular(12), bold(18);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&regular.font, &bold.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  const int split = renderer.getTextAdvanceX(1, "g", EpdFontFamily::BOLD, 'y');
  TextBlock line({"gyp"}, {0}, {EpdFontFamily::REGULAR}, {1}, {static_cast<uint16_t>(split)}, {},
                 {TextBlock::WORD_FLAG_BACKGROUND_BLACK}, {}, {});
  renderer.clearScreen();
  line.render(renderer, 1, 20, 20);
  const auto actual = display.bw;
  renderer.clearScreen();
  renderer.fillRect(20, 20, split + renderer.getTextAdvanceX(1, "yp", EpdFontFamily::REGULAR),
                    renderer.getLineHeight(1), true);
  renderer.drawText(1, 20, 20, "g", false, EpdFontFamily::BOLD);
  renderer.drawText(1, 20 + split, 20, "yp", false, EpdFontFamily::REGULAR);
  EXPECT_EQ(actual, display.bw);
}

TEST(EpubTextGrayscaleTest, RubyOutsideBlackBackgroundKeepsPageForeground) {
  fakeheap::reset(true);
  RasterFont font(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&font.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  TextBlock line({"AB"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {TextBlock::WORD_FLAG_BACKGROUND_BLACK}, {}, {},
                 {"a"});
  const int width = renderer.getTextAdvanceX(1, "AB", EpdFontFamily::REGULAR);
  const int rubyWidth = renderer.getTextAdvanceX(1, "a", EpdFontFamily::SUP);
  const int y = 25 + line.wordYOffset(renderer, 1, 0);
  for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.setRenderMode(mode);
    renderer.clearScreen();
    line.render(renderer, 1, 20, 25);
    const auto actual = display.bw;
    renderer.clearScreen();
    if (mode == GfxRenderer::BW) {
      renderer.fillRect(20, y, width, renderer.getLineHeight(1), true);
      renderer.drawText(1, 20, y, "AB", false);
    }
    renderer.drawText(1, 20 + (width - rubyWidth) / 2, y - renderer.getFontAscenderSize(1), "a", true,
                      EpdFontFamily::SUP);
    EXPECT_EQ(actual, display.bw);
  }
}

TEST(EpubTextGrayscaleTest, CharacterSpacingDecorationsFollowTextWidth) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  for (int8_t spacing : {-5, 5}) {
    for (auto decoration : {EpdFontFamily::UNDERLINE, EpdFontFamily::STRIKETHROUGH}) {
      for (auto size : {EpdFontFamily::REGULAR, EpdFontFamily::SUP}) {
        const auto style = static_cast<EpdFontFamily::Style>(decoration | size);
        TextBlock line({"ABCD"}, {0}, {style}, {}, {}, {}, {}, {}, {}, {}, {}, "", spacing);
        renderer.clearScreen();
        line.render(renderer, 1, 20, 20, true);
        const auto actual = display.bw;
        renderer.clearScreen();
        const int wordY = 20 + line.wordYOffset(renderer, 1, 0);
        renderer.drawText(1, 20, wordY, "ABCD", true, style, BidiUtils::BidiBaseDir::AUTO, spacing);
        const int width = renderer.getTextAdvanceX(1, "ABCD", style, 0, spacing);
        const int glyphHeight = size == EpdFontFamily::SUP ? 7 : 13;
        const int lineY = decoration == EpdFontFamily::UNDERLINE ? wordY + 15 : wordY + 13 - glyphHeight / 2;
        renderer.drawLine(20, lineY, 20 + width, lineY, 3, true);
        EXPECT_EQ(display.bw, actual) << int(spacing) << ' ' << int(style);
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, RealTextRasterMatchesFullAndStripTargets) {
  for (const auto [width, height] : {std::pair{792, 481}, std::pair{800, 481}, std::pair{768, 552}})
    for (bool sd : {false, true})
      for (int size : {12, 20})
        for (int orientation = 0; orientation < 4; ++orientation) {
          SCOPED_TRACE(testing::Message() << sd << ' ' << size << ' ' << orientation);
          fakeheap::reset(true);
          Storage.reset();
          RasterFont fixture(size);
          SdCardFont sdFont;
          HalDisplay display(width, height);
          GfxRenderer renderer(display);
          renderer.begin();
          if (sd) {
            Storage.put("font.cpfont", fixture.file());
            ASSERT_TRUE(sdFont.load("font.cpfont"));
            renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
            renderer.registerSdCardFont(1, &sdFont);
          } else
            renderer.insertFont(1, EpdFontFamily(&fixture.font));
          renderer.setOrientation(GfxRenderer::Orientation(orientation));
          FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
          renderer.setFontCacheManager(&cache);
          const std::vector<std::string> words = {"Abc", "e\u0301", "שלום", "سلام", "一", "\U0001F642", " "};
          const std::vector<int16_t> offsets = {0, 65, 120, 190, 260, 290, 330};
          const std::vector<EpdFontFamily::Style> styles = {
              EpdFontFamily::REGULAR, EpdFontFamily::BOLD,    EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC,
              EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR};
          const auto bw = display.bw;
          for (int feature = 0; feature < 5; ++feature) {
            auto variants = styles;
            BlockStyle blockStyle;
            blockStyle.isRtl = feature == 4;
            for (auto& style : variants)
              style =
                  EpdFontFamily::Style(style | (feature == 0   ? EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH
                                                : feature == 1 ? EpdFontFamily::SMALL_CAPS
                                                : feature == 2 ? EpdFontFamily::SUP
                                                : feature == 3 ? EpdFontFamily::SUB
                                                               : 0));
            const std::array<int, 9> lineY = {-8, 65, 79, 145, 159, 230, 310, 390, 470};
            std::vector<std::unique_ptr<TextBlock>> lines;
            for (size_t i = 0; i < lineY.size(); ++i) {
              lines.emplace_back(new TextBlock(
                  words, offsets, variants,
                  feature == 1 ? std::vector<uint8_t>(words.size(), 1) : std::vector<uint8_t>{},
                  feature == 1 ? std::vector<uint16_t>(words.size(), 12) : std::vector<uint16_t>{},
                  std::vector<uint16_t>(words.size(), 6),
                  std::vector<uint8_t>{0, 0, 0, TextBlock::WORD_FLAG_BACKGROUND_BLACK, 0, 0,
                                       TextBlock::WORD_FLAG_BACKGROUND_BLACK},
                  {}, blockStyle,
                  feature == 4 ? std::vector<std::string>(words.size(), "Ab") : std::vector<std::string>{}));
              ASSERT_TRUE(lines.back()->valid());
            }
            auto draw = [&] {
              for (size_t i = 0; i < lines.size(); ++i) lines[i]->render(renderer, 1, 7, lineY[i], true);
            };
            {
              auto scan = cache.createPrewarmScope();
              draw();
              scan.endScanAndPrewarm();
            }
            EXPECT_EQ(display.bw, bw);
            for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
              renderer.setRenderMode(mode);
              std::vector<uint8_t> full(display.bw.size()), strips(display.bw.size());
              renderer.beginStripTarget(full.data(), 0, display.height);
              renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
              draw();
              renderer.endStripTarget();
              {
                const GfxRenderer::TextCullingScope culling(renderer);
                for (int y = 0; y < display.height; y += 80) {
                  int rows = std::min(80, display.height - y);
                  renderer.beginStripTarget(strips.data() + size_t(y) * display.stride, y, rows);
                  renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
                  draw();
                  renderer.endStripTarget();
                }
              }
              EXPECT_EQ(full, strips) << "feature=" << feature << " mode=" << mode;
              EXPECT_TRUE(std::any_of(full.begin(), full.end(),
                                      [&](auto b) { return b != (mode == GfxRenderer::BW ? 255 : 0); }));
              EXPECT_EQ(display.bw, bw);
              EXPECT_EQ(renderer.getWriteTarget(), display.bw.data());
            }
            renderer.setRenderMode(GfxRenderer::BW);
          }
          renderer.setFontCacheManager(nullptr);
          renderer.removeFont(1);
        }
}
}  // namespace

TEST(EpubTextRaster, VariationSelectorsDoNotDrawOrAdvance) {
  for (const bool sd : {false, true}) {
    SCOPED_TRACE(testing::Message() << "sd=" << sd);
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    SdCardFont sdFont;
    HalDisplay display;
    GfxRenderer renderer(display);
    renderer.begin();
    if (sd) {
      Storage.put("font.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("font.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }

    const auto render = [&renderer, &display](const char* text) {
      renderer.clearScreen();
      renderer.drawText(1, 25, 40, text);
      return display.bw;
    };

    const auto base = render("!");
    const int baseAdvance = renderer.getTextAdvanceX(1, "!", EpdFontFamily::REGULAR);
    for (const char* text : {"!\xE1\xA0\x8B", "!\xEF\xB8\x8E", "!\xEF\xB8\x8F"}) {  // U+180B/U+FE0E/U+FE0F
      EXPECT_EQ(renderer.getTextAdvanceX(1, text, EpdFontFamily::REGULAR), baseAdvance);
      EXPECT_EQ(render(text), base);
    }
    renderer.removeFont(1);
  }
}

TEST(ReaderRuling, LeavesSpaceAroundDescendersAndAccentsWithoutBreakingTheDotPattern) {
  fakeheap::reset(true);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  ReaderRuling ruling{2, 1, 32, 80, 100};
  renderer.clearScreen();
  renderer.drawPixel(33, 37);  // Ink immediately outside the right margin.
  renderer.drawPixel(8, 35);   // Descender just above the rule.
  renderer.drawPixel(17, 39);  // Accent from the following row.
  ruling.draw(renderer, 38);
  for (int x = 1; x < 33; ++x) {
    const bool clear = (x < 6 || x > 10) && (x < 15 || x > 19) && x < 31;
    EXPECT_EQ(renderer.isPixelBlack(x, 37), clear && (x % 2 == 1)) << x;
  }
  EXPECT_TRUE(renderer.isPixelBlack(8, 35));
  EXPECT_TRUE(renderer.isPixelBlack(17, 39));

  renderer.clearScreen();
  ruling.bottom = 34;
  ruling.draw(renderer, 38);
  EXPECT_FALSE(renderer.isPixelBlack(1, 33));
  ruling.draw(renderer, 34);
  EXPECT_TRUE(renderer.isPixelBlack(1, 33));
  EXPECT_FALSE(renderer.isPixelBlack(1, 34));
}

TEST(ReaderRuling, BufferedCollisionChecksMatchPixelClearanceInEveryOrientation) {
  fakeheap::reset(true);
  HalDisplay display(101, 87);
  GfxRenderer renderer(display);
  renderer.begin();
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
    for (const uint8_t level : {1, 2, 3}) {
      for (const int y : {0, 15, h - 1}) {
        renderer.clearScreen();
        for (int x : {0, 29, 30, 31, 32, 63, w - 1}) renderer.drawPixel(x, std::clamp(y + x % 5 - 2, 0, h - 1));
        const auto original = display.bw;
        std::vector<int> expectedDots;
        for (int x = 0; x < w; x += 1 << (3 - level)) {
          bool clear = true;
          for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) clear &= !renderer.isPixelBlack(x + dx, y + dy);
          if (clear) expectedDots.push_back(x);
        }
        ReaderRuling{level, 0, w, h}.draw(renderer, y + 1);
        const auto actual = display.bw;
        std::copy(original.begin(), original.end(), display.bw.begin());
        for (const int x : expectedDots) renderer.drawPixel(x, y);
        EXPECT_EQ(actual, display.bw);
      }
    }
  }
}

TEST(ReaderRuling, CollisionChecksClipToTheActiveStrip) {
  HalDisplay display(65, 41);
  GfxRenderer renderer(display);
  renderer.begin();
  constexpr int origin = 7, rows = 11;
  std::vector<uint8_t> strip(display.stride * rows);
  for (int i = 0; i < static_cast<int>(strip.size()); ++i) strip[i] = i % 11 == 0 ? 0xe7 : 0xff;
  renderer.beginStripTarget(strip.data(), origin, rows);
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    for (int y = -2; y < renderer.getScreenHeight() + 1; y += 3) {
      for (int x = -2; x < renderer.getScreenWidth() + 1; x += 7) {
        bool hasInk = false;
        for (int dy = 0; dy < 5; ++dy)
          for (int dx = 0; dx < 32; ++dx) hasInk |= renderer.isPixelBlack(x + dx, y + dy);
        EXPECT_EQ(renderer.hasBlackPixels(x, y, 32, 5), hasInk);
      }
    }
  }
  renderer.endStripTarget();
}

TEST(ReaderRuling, RetainsRegularLetterMetricsUnderPressureAndRefreshesReusedFontIds) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12), replacement(20);
  Storage.put("metrics.cpfont", fixture.file());
  SdCardFont sd;
  ASSERT_TRUE(sd.load("metrics.cpfont"));
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(sd.getEpdFont()));
  renderer.registerSdCardFont(1, &sd);
  fakeheap::internal.largest = 30 * 1024;
  auto& file = Storage.data("metrics.cpfont");
  GfxRenderer::TextVerticalBounds body{}, extent{};
  ASSERT_TRUE(renderer.getLatinBodyBounds(1, body, extent));
  const auto row = renderer.getTextVerticalBounds(1, "abcdefghijklmnopqrstuvwxyz");
  const auto reads = file.readCalls;
  for (int n = 0; n < 10; ++n) {
    ASSERT_TRUE(renderer.getLatinBodyBounds(1, body, extent));
    EXPECT_EQ(renderer.getTextVerticalBounds(1, "abcdefghijklmnopqrstuvwxyz").bottom, row.bottom);
  }
  EXPECT_EQ(file.readCalls, reads);
  renderer.removeFont(1);
  renderer.insertFont(1, EpdFontFamily(&replacement.font));
  GfxRenderer::TextVerticalBounds newBody{}, newExtent{};
  ASSERT_TRUE(renderer.getLatinBodyBounds(1, newBody, newExtent));
  EXPECT_NE(newBody.bottom, body.bottom);
  EXPECT_NE(renderer.getTextVerticalBounds(1, "a").bottom, row.bottom);
}

TEST(ReaderRuling, MeasuresDeepGlyphsAndOnlyExpandsRuledRows) {
  for (bool sd : {false, true}) {
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    fixture.data.descender = -1;  // The glyph bitmaps actually extend three pixels below the baseline.
    SdCardFont sdFont;
    HalDisplay display;
    GfxRenderer renderer(display);
    renderer.begin();
    if (sd) {
      Storage.put("depth.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("depth.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }
    for (int requested : {10, 35, 50}) {
      const auto row = ReaderRuling::textRow(renderer, 1, "!", requested, 0);
      // The reference stem is 13px tall, and the ink extends another 3px.
      EXPECT_EQ(row.height, std::max(requested, 13 + 2 * (3 + 2) + 1));
      const int stemTop = (row.height - 1 - 13) / 2;
      EXPECT_EQ(row.offset, stemTop);
      renderer.clearScreen();
      renderer.drawText(1, 9, row.offset, "!");
      ReaderRuling{3, 0, 60, 100, 100}.draw(renderer, row.height);
      int first = row.height, last = -1;
      for (int y = 0; y < row.height - 1; ++y)
        for (int x = 0; x < 60; ++x)
          if (renderer.isPixelBlack(x, y)) {
            first = std::min(first, y);
            last = std::max(last, y);
          }
      EXPECT_GE(first, 2);
      EXPECT_GE(row.height - last - 2, 2);
      EXPECT_EQ(first, stemTop);
      for (int x = 0; x < 60; ++x) EXPECT_TRUE(renderer.isPixelBlack(x, row.height - 1));
    }
    renderer.removeFont(1);
  }
}

TEST(ReaderRuling, LowercaseBodyStaysCenteredWhenOtherLettersChange) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  auto& x = fixture.glyphs['x' - 32];
  x.top = 8;
  x.height = 8;
  auto& parenthesis = fixture.glyphs['(' - 32];
  parenthesis.top += 3;
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  const int baseline = fixture.data.ascender;
  for (int height : {renderer.getLineHeight(1), renderer.getLineHeight(1) * 125 / 100, 36}) {
    const auto shortRow = ReaderRuling::textRow(renderer, 1, "xx", height, 0);
    const auto tallRow = ReaderRuling::textRow(renderer, 1, "xHg", height, 0);
    const auto punctuationRow = ReaderRuling::textRow(renderer, 1, "(xx)", height, 0);
    EXPECT_EQ(shortRow.height, punctuationRow.height);
    EXPECT_EQ(shortRow.offset, punctuationRow.offset);
    EXPECT_EQ(shortRow.height, tallRow.height);
    EXPECT_EQ(shortRow.offset, tallRow.offset);
    GfxRenderer::TextVerticalBounds referenceBody, extent;
    ASSERT_TRUE(renderer.getLatinBodyBounds(1, referenceBody, extent));
    EXPECT_EQ(referenceBody.top, renderer.getTextVerticalBounds(1, "h").top);
    EXPECT_EQ(referenceBody.bottom, baseline);
    const int above = shortRow.offset + referenceBody.top;
    const int below = shortRow.height - 1 - shortRow.offset - baseline;
    EXPECT_LE(std::abs(above - below), 1);
    const auto tall = renderer.getTextVerticalBounds(1, "xHg");
    EXPECT_GE(tallRow.offset + tall.top, 2);
    EXPECT_GE(tallRow.height - 1 - tallRow.offset - tall.bottom, 2);
    TextBlock block({"xHg"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {});
    const auto epub = block.ruledLayout(renderer, 1, height);
    EXPECT_EQ(epub.height, tallRow.height);
    EXPECT_EQ(epub.offset, tallRow.offset);
  }
}

TEST(ReaderRuling, OrdinaryDescenderKeepsSharedPitchAndClearance) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  auto& x = fixture.glyphs['x' - 32];
  x.top = x.height = 8;
  auto& g = fixture.glyphs['g' - 32];
  g.top = 8;
  g.height = 18;
  // A deeper tail outside the reference sample can require an even taller row.
  auto& y = fixture.glyphs['y' - 32];
  y.top = 8;
  y.height = 20;
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (const char* text : {"xx", "xg"}) {
    for (int requested : {33, 42}) {
      const auto row = ReaderRuling::textRow(renderer, 1, text, requested, 0);
      // A centered 13px stem leaves room for a 10px tail and 2 blank pixels.
      EXPECT_EQ(row.height, std::max(requested, 38));
      EXPECT_EQ(row.offset, (row.height - 1 - 13) / 2);
      EXPECT_GE(row.height - 1 - row.offset - (fixture.data.ascender + 10), 2);
    }
  }
  const auto outlier = ReaderRuling::textRow(renderer, 1, "xy", 33, 0);
  EXPECT_EQ(outlier.height, 42);
  EXPECT_EQ(outlier.offset, (outlier.height - 1 - 13) / 2);
  EXPECT_GE(outlier.height - 1 - outlier.offset - renderer.getTextVerticalBounds(1, "xy").bottom, 2);
}

TEST(ReaderRuling, CentersQuotedCapitalsAndKeepsMixedProseOnItsBaseline) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  auto& x = fixture.glyphs['x' - 32];
  x.top = x.height = 8;
  fixture.glyphs[' ' - 32].width = fixture.glyphs[' ' - 32].height = 0;
  // Capital X and the exclamation end at the baseline. The guide dot is shorter.
  for (const char cp : {'X', '!'}) {
    fixture.glyphs[cp - 32].top = 10;
    fixture.glyphs[cp - 32].height = 10;
  }
  auto& dot = fixture.glyphs[fixture.intervals[1].offset];
  dot.top = 6;
  dot.height = 2;
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (const int requested : {22, 36}) {
    const auto caps = ReaderRuling::textRow(renderer, 1, "XXX! · XX", requested, 0);
    const auto plain = ReaderRuling::textRow(renderer, 1, "xxx", requested, 0);
    const auto mixed = ReaderRuling::textRow(renderer, 1, "XXX! The", requested, 0);
    const auto visible = renderer.getTextVerticalBounds(1, "XXX! · XX");
    EXPECT_EQ(caps.height, plain.height);
    EXPECT_EQ(mixed.offset, plain.offset);
    EXPECT_NE(caps.offset, mixed.offset);
    EXPECT_LE(std::abs(caps.offset + visible.top - (caps.height - 1 - caps.offset - visible.bottom)), 1);
    EXPECT_GE(caps.offset + visible.top, 2);
    EXPECT_GE(caps.height - 1 - caps.offset - visible.bottom, 2);

    TextBlock line({"XXX!", "XX"}, {0, 45}, {EpdFontFamily::BOLD, EpdFontFamily::REGULAR}, {}, {}, {}, {}, {});
    const auto epub = line.ruledLayout(renderer, 1, requested);
    EXPECT_EQ(epub.height, caps.height);
    EXPECT_EQ(epub.offset, caps.offset);
  }
}

TEST(ReaderRuling, FontWithoutLatinMeasuresItsOwnInk) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  fixture.intervals = {{0x4e00, 0x4e00, 0}, {0xfffd, 0xfffd, 1}};
  fixture.data.intervals = fixture.intervals.data();
  fixture.data.intervalCount = fixture.intervals.size();
  fixture.glyphs[1].height = 3;
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  GfxRenderer::TextVerticalBounds body, extent;
  EXPECT_FALSE(renderer.getLatinBodyBounds(1, body, extent));
  const auto row = ReaderRuling::textRow(renderer, 1, "一", 35, 0);
  const auto ink = renderer.getTextVerticalBounds(1, "一");
  EXPECT_EQ(row.offset, (35 - 1 - ink.top - ink.bottom) / 2);
}

TEST(ReaderRuling, MeasuringSyntheticGreekLeavesFramebufferUntouched) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  fixture.data.intervalCount = 1;
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.clearScreen();
  const auto blank = display.bw;
  const auto ink = renderer.getTextVerticalBounds(1, "εωΓ");
  EXPECT_GT(ink.bottom, ink.top);
  EXPECT_EQ(display.bw, blank);
}

TEST(ReaderRuling, AnchorsToLayoutHeight) {
  fakeheap::reset(true);
  RasterFont fixture(12);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  ReaderRuling ruling{3, 10, 30, 110, 150};
  renderer.clearScreen();
  TextBlock line({"A"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {});
  ruling.drawBlock(renderer, line, 1, 4);
  const int defaultHeight = (renderer.getLineHeight(1) * 150 + 50) / 100;
  EXPECT_TRUE(renderer.isPixelBlack(10, 4 + defaultHeight - 1));
  BlockStyle style;
  style.lineHeight = 19;
  line.setBlockStyle(style);
  ruling.drawBlock(renderer, line, 1, 40);
  EXPECT_TRUE(renderer.isPixelBlack(10, 58));
  EXPECT_FALSE(renderer.isPixelBlack(10, 59));
}

TEST(AbsoluteImageRaster, TextCoverageMatchesOverlayShadesInCompletePlanes) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS}) {
      const auto draw = [&] { renderer.drawText(1, 25, 40, "Book cover", true, style); };
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      draw();
      const auto base = display.bw;
      std::array<std::vector<uint8_t>, 2> masks;
      int index = 0;
      for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
        renderer.setRenderMode(mode);
        renderer.clearScreen(0);
        draw();
        masks[index++] = display.bw;
      }
      ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
      index = 0;
      for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
        display.bw = base;
        renderer.setRenderMode(mode);
        draw();
        auto expected = base;
        for (size_t byte = 0; byte < base.size(); ++byte)
          expected[byte] |= index ? masks[1][byte] & ~masks[0][byte] : masks[0][byte];
        EXPECT_EQ(display.bw, expected);
        EXPECT_NE(display.bw, base);
        ++index;
      }
      renderer.setRenderMode(GfxRenderer::BW);
      EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
    }
  }
  EXPECT_EQ(display.canceled, 8);
  display.absoluteSupported = false;
  EXPECT_FALSE(renderer.supportsAbsoluteGrayscale());
  EXPECT_FALSE(renderer.displayAbsoluteGrayscaleBase());
  EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
}

TEST(AbsoluteImageRaster, BitmapPlanesPreserveFourTonesAndWhiteMargins) {
  fakeheap::reset(true);
  Storage.reset();
  // Two identical bottom-up rows of black/dark/light/white, padded to four bytes.
  auto data = std::make_shared<HostFileData>();
  data->bytes.resize(78, 0);
  auto put16 = [&](int offset, uint16_t value) { memcpy(data->bytes.data() + offset, &value, 2); };
  auto put32 = [&](int offset, uint32_t value) { memcpy(data->bytes.data() + offset, &value, 4); };
  put16(0, 0x4d42);
  put32(2, 78);
  put32(10, 70);
  put32(14, 40);
  put32(18, 4);
  put32(22, 2);
  put16(26, 1);
  put16(28, 2);
  put32(34, 8);
  put32(46, 4);
  for (int level = 0; level < 4; ++level)
    for (int channel = 0; channel < 3; ++channel) data->bytes[54 + level * 4 + channel] = level * 85;
  data->bytes[70] = data->bytes[74] = 0x1b;
  HalFile file(data);
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  HalDisplay display(8, 4);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
  int plane = 0;
  for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    renderer.clearScreen();
    renderer.setRenderMode(mode);
    ASSERT_TRUE(renderer.drawBitmap(bitmap, 0, 3, 8, 4));
    // The first file row lies below the screen. The second must still be read.
    EXPECT_EQ(display.bw[0], 0xff);
    EXPECT_EQ(display.bw[1], 0xff);
    EXPECT_EQ(display.bw[2], 0xff);
    EXPECT_EQ(display.bw[3], plane++ == 0 ? 0x5f : 0x3f);
  }
  // A truncated second pass must be reported, allowing the caller to cancel it.
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  data->readFailAt = 74;
  EXPECT_FALSE(renderer.drawBitmap(bitmap, 0, 0, 8, 4));
  renderer.setRenderMode(GfxRenderer::BW);
  EXPECT_EQ(display.canceled, 1);
  file.close();
}

TEST(RetainedGrayscaleOverlay, RotatesTheRegionAndRejectsInvalidBoundsWithoutPainting) {
  fakeheap::reset(true);
  HalDisplay display(792, 481);
  GfxRenderer renderer(display);
  renderer.begin();
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    ASSERT_TRUE(renderer.displayRetainedGrayscaleOverlay(7, 11, 24, 35, true));
    const auto& region = display.retainedOverlays.back();
    EXPECT_TRUE(region.restore);
    if (orientation == GfxRenderer::Portrait) {
      EXPECT_EQ(region.x, 11);
      EXPECT_EQ(region.y, 450);
      EXPECT_EQ(region.width, 35);
      EXPECT_EQ(region.height, 24);
    } else if (orientation == GfxRenderer::PortraitInverted) {
      EXPECT_EQ(region.x, 746);
      EXPECT_EQ(region.y, 7);
      EXPECT_EQ(region.width, 35);
      EXPECT_EQ(region.height, 24);
    } else if (orientation == GfxRenderer::LandscapeClockwise) {
      EXPECT_EQ(region.x, 761);
      EXPECT_EQ(region.y, 435);
      EXPECT_EQ(region.width, 24);
      EXPECT_EQ(region.height, 35);
    } else {
      EXPECT_EQ(region.x, 7);
      EXPECT_EQ(region.y, 11);
      EXPECT_EQ(region.width, 24);
      EXPECT_EQ(region.height, 35);
    }
    const auto calls = display.retainedOverlays.size();
    EXPECT_FALSE(renderer.displayRetainedGrayscaleOverlay(-1, 0, 1, 1));
    EXPECT_FALSE(renderer.displayRetainedGrayscaleOverlay(0, 0, 0, 1));
    EXPECT_FALSE(renderer.displayRetainedGrayscaleOverlay(0, 0, renderer.getScreenWidth() + 1, 1));
    EXPECT_FALSE(renderer.displayRetainedGrayscaleOverlay(0, renderer.getScreenHeight(), 1, 1));
    EXPECT_EQ(display.retainedOverlays.size(), calls);
  }
}

// A font switch begins with no resident glyphs. A wide replacement glyph makes
// the cold scan fit fewer words than the final, correctly measured preview.
TEST(EpubTextGrayscaleTest, ColdSdSamplePreviewMatchesFullyLoadedFont) {
  for (int size : {12, 20}) {
    for (int width : {160, 280}) {
      for (bool focus : {false, true}) {
        for (bool guide : {false, true}) {
          SCOPED_TRACE(testing::Message() << size << " width=" << width << " focus=" << focus << " guide=" << guide);
          fakeheap::reset(false);
          Storage.reset();
          RasterFont fixture(size);
          fixture.glyphs.back().advanceX = size * 16 * 4;
          SdCardFont sdFont;
          Storage.put("preview.cpfont", fixture.file());
          ASSERT_TRUE(sdFont.load("preview.cpfont"));
          HalDisplay display;
          GfxRenderer renderer(display);
          renderer.begin();
          renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
          renderer.registerSdCardFont(1, &sdFont);
          renderer.insertFont(2, EpdFontFamily(&fixture.font));
          FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
          renderer.setFontCacheManager(&cache);
          SampleReaderPreviewModel model;
          ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
          auto draw = [&](int font) {
            model.renderText(renderer, font, 10, 10, width, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), focus,
                             guide, true, 85);
          };
          renderer.clearScreen();
          draw(2);
          const auto expected = display.bw;
          renderer.clearScreen();
          const auto blank = display.bw;
          auto scope = cache.createPrewarmScope();
          draw(1);
          EXPECT_EQ(display.bw, blank);  // Scanning must not paint the display.
          ASSERT_TRUE(scope.endScanAndPrewarm());
          draw(1);
          EXPECT_NE(display.bw, blank);
          EXPECT_TRUE(display.bw == expected);
        }
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, NightModeImagesKeepPolarityAndMidtonesOnRepeatedDraws) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (bool grayscale : {false, true}) {
      for (int level = 0; level < 4; ++level) {
        SCOPED_TRACE(testing::Message() << orientation << ' ' << grayscale << ' ' << level);
        HalDisplay display(800, 480);
        GfxRenderer renderer(display);
        renderer.begin();
        renderer.setOrientation(GfxRenderer::Orientation(orientation));
        // The light-mode monochrome image is the expected Night Mode output.
        renderer.clearScreen(255);
        DirectPixelWriter writer;
        const auto draw = [&](bool imageGrayscale) {
          writer.init(renderer, imageGrayscale);
          for (int y = 13; y < 29; ++y) {
            writer.beginRow(y);
            for (int x = 9; x < 25; ++x) writer.writePixel(x, level);
          }
          renderer.preserveImagePolarity(9, 13, 16, 16);
        };
        draw(false);
        const auto expected = display.bw;
        display.inverted = true;
        renderer.clearScreen(255);
        for (int redraw = 0; redraw < 3; ++redraw) {
          draw(grayscale);
          // Apply output inversion only to the image rectangle for comparison;
          // surrounding text/background is expected to change in Night Mode.
          renderer.invertRect(9, 13, 16, 16);
          EXPECT_EQ(display.bw, expected) << redraw;
          renderer.invertRect(9, 13, 16, 16);
        }
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, CachedNightModeImagesMatchMonochromeAcrossStoragePaths) {
  for (bool psram : {false, true}) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      SCOPED_TRACE(testing::Message() << psram << ' ' << orientation);
      fakeheap::reset(psram);
      Storage.reset();
      ImageBlock::clearSessionRenderFailures();
      std::vector<uint8_t> pixels(4 + 16 * 4, 0x33);
      pixels[0] = pixels[2] = 16;
      pixels[1] = pixels[3] = 0;
      Storage.put("image_mono_v1.pxc", pixels);
      HalDisplay display(800, 480);
      GfxRenderer renderer(display);
      renderer.begin();
      renderer.setOrientation(GfxRenderer::Orientation(orientation));
      ImageBlock image("image.png", "source.png", 16, 16);
      renderer.clearScreen(255);
      image.render(renderer, 9, 13, true, false);
      const auto expected = display.bw;
      display.inverted = true;
      renderer.clearScreen(255);
      for (int redraw = 0; redraw < 3; ++redraw) {
        image.render(renderer, 9, 13, true, true);
        renderer.invertRect(9, 13, 16, 16);
        EXPECT_EQ(display.bw, expected) << redraw;
        renderer.invertRect(9, 13, 16, 16);
      }
      ImageBlock::clearSessionRenderFailures();
    }
  }
}

namespace {
class TaggedPageElement final : public PageElement {
 public:
  explicit TaggedPageElement(PageElementTag tag) : PageElement(0, 0), tag(tag) {}
  void render(GfxRenderer&, int, int, int, bool) override {}
  bool serialize(Print&) override { return true; }
  PageElementTag getTag() const override { return tag; }

 private:
  PageElementTag tag;
};
}  // namespace
TEST(PageTextDetection, OnlyTextAndTablesNeedTextAntialiasing) {
  Page page;
  EXPECT_FALSE(page.hasText());
  for (auto tag : {TAG_PageImage, TAG_PageHorizontalRule}) {
    page.elements.emplace_back(std::make_unique<TaggedPageElement>(tag));
    EXPECT_FALSE(page.hasText());
  }
  for (auto tag : {TAG_PageLine, TAG_PageTableFragment}) {
    page.elements.emplace_back(std::make_unique<TaggedPageElement>(tag));
    EXPECT_TRUE(page.hasText());
    page.elements.pop_back();
  }
}

TEST(ReaderRefreshScope, PageAndAaUseReadingWaveformThenMenuReturnsToUi) {
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.displayBuffer();  // Home before entering a reader.
  {
    const ReaderUtils::ReaderRefreshScope reading(renderer);
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    renderer.displayGrayBuffer();
  }
  renderer.displayBuffer();  // Menu pushed over the reader.
  {
    const ReaderUtils::ReaderRefreshScope reading(renderer);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);  // Return and periodic cleanup.
  }
  renderer.displayBuffer();  // Leave the book.
  EXPECT_EQ(display.refreshHints, (std::vector<bool>{false, true, true, false, true, false}));
}

TEST(ReaderRefreshScope, EarlyReturnDoesNotLeaveReaderHintEnabled) {
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  const auto failedPage = [&]() {
    const ReaderUtils::ReaderRefreshScope reading(renderer);
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    return false;  // For example, a failed grayscale allocation or image read.
  };
  EXPECT_FALSE(failedPage());
  renderer.displayBuffer();
  EXPECT_EQ(display.refreshHints, (std::vector<bool>{true, false}));
}

TEST(ReaderRefreshScope, BufferOnlyCompositionLeavesRefreshPolicyAlone) {
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  for (bool initial : {false, true}) {
    display.setReaderRefresh(initial);
    {
      const ReaderUtils::ReaderRefreshScope bufferOnly(renderer, GfxRenderer::RefreshContent::Image, false);
      EXPECT_EQ(display.readerRefresh, initial);
    }
    EXPECT_EQ(display.readerRefresh, initial);
  }
}

TEST(ReaderRefreshScope, CombinedTextIsOptedInOnlyForTextPages) {
  HalDisplay display(800, 480);
  display.combinedTextSupported = true;
  GfxRenderer renderer(display);
  EXPECT_FALSE(renderer.combinesGrayscaleBase());
  {
    const ReaderUtils::ReaderRefreshScope text(renderer);
    EXPECT_TRUE(display.readerRefresh);
    EXPECT_TRUE(renderer.supportsTextGrayscale());
    EXPECT_TRUE(renderer.combinesGrayscaleBase());
  }
  {
    const ReaderUtils::ReaderRefreshScope images(renderer, GfxRenderer::RefreshContent::Image);
    EXPECT_FALSE(display.readerRefresh);
    EXPECT_FALSE(renderer.combinesGrayscaleBase());
    EXPECT_TRUE(renderer.supportsTextGrayscale());
  }
  EXPECT_FALSE(display.readerRefresh);
}

TEST(ReaderRefreshScope, EegoUsesBothGraysForImagesAndOptionalText) {
  using Scope = ReaderUtils::ReaderRefreshScope;
  HalDisplay display(800, 480);
  display.alwaysCombined = true;
  GfxRenderer renderer(display);
  for (auto content : {Scope::text(false), Scope::Content::Image, Scope::text(true)}) {
    {
      const Scope reading(renderer, content);
      EXPECT_TRUE(display.readerRefresh);
      EXPECT_EQ(display.fullTone, content != Scope::Content::Text);
    }
    EXPECT_FALSE(display.readerRefresh);
    EXPECT_FALSE(display.fullTone);
  }
}

TEST(ReaderRefreshScope, SeparatePanelsAndAllocationFallbackRetainReaderWaveform) {
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  // Models Picco, C3, and Pro/Classic when the SDK's PSRAM allocation fails.
  for (auto content : {GfxRenderer::RefreshContent::Image, GfxRenderer::RefreshContent::Text}) {
    const ReaderUtils::ReaderRefreshScope reading(renderer, content);
    EXPECT_TRUE(display.readerRefresh);
    EXPECT_FALSE(renderer.combinesGrayscaleBase());
    EXPECT_TRUE(renderer.supportsTextGrayscale());
  }
}

TEST(ReaderRefreshScope, NightTextUsesTheActivePanelsCapabilities) {
  HalDisplay display(800, 480);
  display.inverted = true;
  GfxRenderer renderer(display);
  const ReaderUtils::ReaderRefreshScope reading(renderer);
  EXPECT_FALSE(renderer.supportsTextGrayscale());
  display.invertedOverlaySupported = true;
  EXPECT_TRUE(renderer.supportsTextGrayscale());
  display.overlaySupported = false;
  EXPECT_FALSE(renderer.supportsTextGrayscale());
}

TEST(EpubTextGrayscaleTest, CullingIncludesFallbackSyntheticInkAndDecorations) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12), fallback(40);
  // ASCII primary, taller native CJK fallback, and synthetic Greek/blocks.
  fixture.data.intervalCount = 1;
  fallback.intervals = {{0x4e00, 0x4e00, 0}};
  fallback.data.intervals = fallback.intervals.data();
  fallback.data.intervalCount = 1;
  HalDisplay display(800, 481);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font, nullptr, nullptr, nullptr, &fallback.font));
  const auto underline = EpdFontFamily::UNDERLINE;
  TextBlock line({"一", "εωΓ■█🙂", "   ", "Test"}, {0, 120, 240, 360},
                 {underline, underline, EpdFontFamily::REGULAR, EpdFontFamily::STRIKETHROUGH}, {}, {}, {},
                 {0, 0, TextBlock::WORD_FLAG_BACKGROUND_BLACK, 0}, {});
  ASSERT_TRUE(line.valid());
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    for (bool black : {false, true}) {
      auto render = [&](bool optimized) {
        std::vector<uint8_t> result(display.bw.size() * 2);
        auto replay = [&] {
          for (int plane = 0; plane < 2; ++plane) {
            renderer.setRenderMode(plane ? GfxRenderer::GRAYSCALE_MSB : GfxRenderer::GRAYSCALE_LSB);
            for (int row = 0; row < display.height; row += 17) {
              renderer.beginStripTarget(result.data() + display.bw.size() * plane + row * display.stride, row,
                                        std::min(17, display.height - row));
              renderer.beginTextClip(10, 10, 400, 150);
              line.render(renderer, 1, 0, 78, black);
              renderer.endTextClip();
              renderer.endStripTarget();
            }
          }
        };
        if (optimized) {
          const GfxRenderer::TextCullingScope culling(renderer);
          replay();
        } else
          replay();
        return result;
      };
      const auto expected = render(false);
      EXPECT_TRUE(std::any_of(expected.begin(), expected.end(), [](uint8_t b) { return b != 0; }));
      EXPECT_EQ(render(true), expected) << orientation << ' ' << black;
    }
  }
}
