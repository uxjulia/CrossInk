#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
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
  for (bool sd : {false, true})
    for (int size : {12, 20})
      for (int orientation = 0; orientation < 4; ++orientation) {
        SCOPED_TRACE(testing::Message() << sd << ' ' << size << ' ' << orientation);
        fakeheap::reset(true);
        Storage.reset();
        RasterFont fixture(size);
        SdCardFont sdFont;
        HalDisplay display(orientation % 2 ? 800 : 792, 481);
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
            style = EpdFontFamily::Style(style | (feature == 0 ? EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH
                                                  : feature == 1 ? EpdFontFamily::SMALL_CAPS
                                                  : feature == 2 ? EpdFontFamily::SUP
                                                  : feature == 3 ? EpdFontFamily::SUB
                                                                 : 0));
          TextBlock line(words, offsets, variants,
                         feature == 1 ? std::vector<uint8_t>(words.size(), 1) : std::vector<uint8_t>{},
                         feature == 1 ? std::vector<uint16_t>(words.size(), 12) : std::vector<uint16_t>{},
                         std::vector<uint16_t>(words.size(), 6),
                         std::vector<uint8_t>(words.size(), TextBlock::WORD_FLAG_BACKGROUND_BLACK), {}, blockStyle,
                         feature == 4 ? std::vector<std::string>(words.size(), "Ab") : std::vector<std::string>{});
          ASSERT_TRUE(line.valid());
          auto draw = [&] {
            for (int y : {-8, 65, 79, 145, 159, 230, 310, 390, 470}) line.render(renderer, 1, 7, y, true);
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
            for (int y = 0; y < display.height; y += 80) {
              int rows = std::min(80, display.height - y);
              renderer.beginStripTarget(strips.data() + size_t(y) * display.stride, y, rows);
              renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
              draw();
              renderer.endStripTarget();
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

TEST(AbsoluteImageRaster, TextMatchesBlackWhiteInBothPlanesAndCancellationResetsMode) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    renderer.clearScreen();
    renderer.drawText(1, 25, 40, "Book cover");
    const auto expected = display.bw;
    ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
    for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.clearScreen();
      renderer.setRenderMode(mode);
      renderer.drawText(1, 25, 40, "Book cover");
      EXPECT_EQ(display.bw, expected);
    }
    renderer.setRenderMode(GfxRenderer::BW);
    EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
  }
  EXPECT_EQ(display.canceled, 4);
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
      std::vector<uint8_t> pixels(4 + 16 * 4, 0x1B);
      pixels[0] = pixels[2] = 16;
      pixels[1] = pixels[3] = 0;
      Storage.put("image_v2.pxc", pixels);
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
