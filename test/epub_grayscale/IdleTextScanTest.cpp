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
}
