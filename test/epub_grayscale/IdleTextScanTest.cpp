#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "lib/Epub/Epub/blocks/ImageBlock.cpp"

bool ImageDecoderFactory::supportsMemorySource(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }

TEST(IdleTextScanTest, RichTextCollectsExactRunsWithoutMetricsKerningOrRasterization) {
  fakeheap::reset(true);
  Storage.reset();
  struct InstrumentedFont {
    EpdGlyph glyph{};
    uint8_t pixels[16]{};
    int metrics = 0, raster = 0, kerning = 0;
  } font;
  font.glyph.width = 5;
  font.glyph.height = 7;
  font.glyph.advanceX = 6 * 16;
  font.glyph.top = 7;
  font.glyph.dataLength = 9;
  EpdFontData data{};
  data.ascender = 8;
  data.advanceY = 10;
  data.is2Bit = true;
  data.glyphMissCtx = &font;
  data.dynamicGlyphHandler = [](void* context, uint32_t cp) {
    auto& f = *static_cast<InstrumentedFont*>(context);
    ++f.metrics;
    f.glyph.dataOffset = cp;
    return static_cast<const EpdGlyph*>(&f.glyph);
  };
  data.bitmapHandler = [](void* context, const EpdGlyph*) {
    auto& f = *static_cast<InstrumentedFont*>(context);
    ++f.raster;
    return static_cast<const uint8_t*>(f.pixels);
  };
  data.kerningHandler = [](void* context, uint32_t, uint32_t) -> int8_t {
    ++static_cast<InstrumentedFont*>(context)->kerning;
    return 0;
  };
  EpdFont regular(&data);
  EpdFontData boldData = data;
  EpdFont bold(&boldData);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&regular, &bold));
  FontCacheManager manager(renderer.getFontMap(), renderer.getSdCardFonts());
  renderer.setFontCacheManager(&manager);
  const auto strike = static_cast<EpdFontFamily::Style>(EpdFontFamily::STRIKETHROUGH | EpdFontFamily::UNDERLINE);
  TextBlock line({"AB", "CD", " "}, {0, 20, 40}, {strike, EpdFontFamily::RUBY_CONTINUE, EpdFontFamily::REGULAR},
                 {1, 0, 0}, {6, 0, 0}, {10, 0, 0}, {0, 0, TextBlock::WORD_FLAG_BACKGROUND_BLACK}, {}, {},
                 {"ruby", "ignored", ""});
  ASSERT_TRUE(line.valid());
  const auto before = display.bw;
  std::vector<uint8_t> strip(display.stride * 80);
  const GfxRenderer::TextCullingScope culling(renderer);
  renderer.beginStripTarget(strip.data(), 0, 80);
  // Populate off-strip bounds first. Scanning must still visit every run.
  line.render(renderer, 1, 0, 0, true);
  font.metrics = font.kerning = font.raster = 0;
  FontCacheManager::IdleScalableJob job;
  {
    auto scan = manager.createPrewarmScope(FontCacheManager::PreparationPolicy::IdleScalable);
    line.render(renderer, 1, 0, 0, true);
    EXPECT_EQ(font.metrics, 0);
    EXPECT_EQ(font.kerning, 0);
    EXPECT_EQ(font.raster, 0);
    EXPECT_EQ(display.bw, before);
    scan.endScanForIdle(job);
  }
  const auto recorded = [&](uint32_t cp, int style) {
    for (unsigned i = 0; i < job.count; ++i)
      if ((job.codepoints[i] & ((1u << 21) - 1)) == cp && ((job.codepoints[i] >> 21) & 3) == unsigned(style))
        return true;
    return false;
  };
  for (const uint32_t cp : {'r', 'u', 'b', 'y', 'B', 'C', 'D', ' '}) EXPECT_TRUE(recorded(cp, 0));
  EXPECT_TRUE(recorded('A', 1));
  EXPECT_TRUE(recorded(0xB7, 0));
  EXPECT_FALSE(recorded('i', 0));  // Ruby continuation must not emit a second ruby run.
  EXPECT_EQ(manager.prewarmIdleSlice(job, nullptr, nullptr), FontCacheManager::IdleResult::Complete);
  EXPECT_GT(font.metrics, 0);
  EXPECT_GT(font.raster, 0);  // Instrumentation is live, not a scan-only stub.
  renderer.endStripTarget();
}

TEST(IdleTextScanTest, StripCullingPreservesPixelsAndSkipsScalableRunWork) {
  fakeheap::reset(true);
  Storage.reset();
  struct Font {
    EpdGlyph glyph{6, 9, 7 * 16, -2, 8, 14, 0};
    uint8_t pixels[14];
    unsigned metrics = 0, raster = 0, kern = 0;
    int rise = 0;
    Font() { memset(pixels, 0x1B, sizeof(pixels)); }
  } font;
  EpdFontData data{};
  data.ascender = 8;
  data.advanceY = 12;
  data.is2Bit = true;
  data.glyphMissCtx = &font;
  data.dynamicGlyphHandler = [](void* context, uint32_t cp) -> const EpdGlyph* {
    auto& f = *static_cast<Font*>(context);
    ++f.metrics;
    f.glyph.top = 8 + f.rise + (cp == 0x301 ? 12 : 0);
    return &f.glyph;
  };
  data.bitmapHandler = [](void* context, const EpdGlyph*) -> const uint8_t* {
    auto& f = *static_cast<Font*>(context);
    ++f.raster;
    return f.pixels;
  };
  data.kerningHandler = [](void* context, uint32_t, uint32_t) -> int8_t {
    ++static_cast<Font*>(context)->kern;
    return -2;
  };
  EpdFont regular(&data);
  HalDisplay display(800, 481);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&regular));
  std::vector<std::unique_ptr<TextBlock>> lines;
  const std::vector<std::string> words = {"Abcd", "e\u0301efg", "hijk", "lmno"};
  const std::vector<int16_t> positions = {0, 120, 240, 360};
  const auto decorated = static_cast<EpdFontFamily::Style>(EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH);
  const std::vector<EpdFontFamily::Style> styles = {decorated, EpdFontFamily::SUP, EpdFontFamily::SMALL_CAPS,
                                                    EpdFontFamily::SUB};
  for (int i = 0; i < 20; ++i) {
    lines.emplace_back(new TextBlock(words, positions, styles, {1, 0, 0, 0}, {8, 0, 0, 0}, {30, 0, 0, 0}, {}, {}, {},
                                     {"ruby", "", "", ""}));
    ASSERT_TRUE(lines.back()->valid());
  }
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    // Reusing the same page with changed font metrics must start fresh bounds.
    for (font.rise = 0; font.rise <= 30; font.rise += 30) {
      for (bool black : {false, true}) {
        auto render = [&](bool optimized) {
          std::vector<uint8_t> result(display.bw.size() * 2);
          const auto replay = [&] {
            for (int plane = 0; plane < 2; ++plane) {
              renderer.setRenderMode(plane ? GfxRenderer::GRAYSCALE_MSB : GfxRenderer::GRAYSCALE_LSB);
              for (int y = 0; y < display.height; y += 80) {
                renderer.beginStripTarget(result.data() + display.bw.size() * plane + y * display.stride, y,
                                          std::min(80, display.height - y));
                for (int i = 0; i < int(lines.size()); ++i) lines[i]->render(renderer, 1, 6, 5 + i * 24, black);
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
        font.metrics = font.raster = font.kern = 0;
        const auto expected = render(false);
        const auto fullMetrics = font.metrics;
        const auto fullKern = font.kern;
        font.metrics = font.raster = font.kern = 0;
        EXPECT_EQ(render(true), expected) << orientation << ' ' << font.rise << ' ' << black;
        EXPECT_LT(font.metrics, fullMetrics / 2);
        EXPECT_LT(font.kern, fullKern / 2);
        if (!font.rise && black)
          printf("orientation=%d metrics=%u->%u kerning=%u->%u\n", orientation, fullMetrics, font.metrics, fullKern,
                 font.kern);
      }
    }
  }
}

TEST(IdleTextScanTest, TransientGlyphLookupFailureDoesNotCullRecoveredInk) {
  for (bool scalable : {false, true}) {
    for (bool combining : {false, true}) {
      fakeheap::reset(true);
      Storage.reset();
      struct Font {
        EpdGlyph glyph{6, 9, 7 * 16, 0, 8, 14, 0};
        uint8_t pixels[14];
        bool fail = false;
        uint32_t failingCp = 0;
        Font() { memset(pixels, 0x55, sizeof(pixels)); }
      } font;
      font.failingCp = combining ? 0x301 : 'A';
      EpdFontData data{};
      data.ascender = 8;
      data.advanceY = 12;
      data.is2Bit = true;
      data.glyphMissCtx = &font;
      auto lookup = [](void* context, uint32_t cp) -> const EpdGlyph* {
        auto& f = *static_cast<Font*>(context);
        if (f.fail && cp == f.failingCp) return nullptr;
        // The recovered glyph reaches an earlier strip than the replacement.
        f.glyph.top = cp == f.failingCp ? 148 : 8;
        return &f.glyph;
      };
      if (scalable)
        data.dynamicGlyphHandler = lookup;
      else
        data.glyphMissHandler = lookup;
      data.bitmapHandler = [](void* context, const EpdGlyph*) -> const uint8_t* {
        return static_cast<Font*>(context)->pixels;
      };
      EpdFont regular(&data);
      HalDisplay display(800, 481);
      GfxRenderer renderer(display);
      renderer.begin();
      renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
      renderer.insertFont(1, EpdFontFamily(&regular));
      TextBlock line({combining ? "A\u0301" : "A"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {});
      ASSERT_TRUE(line.valid());
      auto render = [&](bool optimized) {
        std::vector<uint8_t> result(display.bw.size());
        const auto replay = [&] {
          renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
          for (int y = 0; y < display.height; y += 80) {
            font.fail = y == 0;
            renderer.beginStripTarget(result.data() + y * display.stride, y, std::min(80, display.height - y));
            line.render(renderer, 1, 20, 240);
            renderer.endStripTarget();
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
      ASSERT_TRUE(std::any_of(expected.begin(), expected.end(), [](uint8_t b) { return b != 0; }));
      EXPECT_EQ(render(true), expected) << "scalable=" << scalable << " combining=" << combining;
    }
  }
}
