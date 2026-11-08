#include <Arduino.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

// Hyphen dictionary availability is outside allocation policy. Exercise the
// production non-hyphenating layout, real bidi, TextBlock and serialization.
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

struct TextLayoutBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  std::vector<uint8_t> layout(int font, int scenario, bool* result = nullptr) {
    BlockStyle style;
    if (scenario == 1) {
      style.directionDefined = true;
      style.isRtl = true;
    }
    if (scenario == 3) {
      style.marginLeft = 12;
      style.marginRight = 12;
    }
    ParsedText text(false, false, false, scenario == 4, scenario == 5, 0, style);
    const std::vector<std::string> words =
        scenario == 1 ? std::vector<std::string>{"שלום", "עולם", "עברית", "123"}
        : scenario == 2
            ? std::vector<std::string>{"日", "本", "語", "読", "書"}
            : std::vector<std::string>{"A", "paragraph", "with", "different", "word", "lengths", "and", "punctuation."};
    for (int i = 0; i < 400; ++i)
      text.addWord(words[i % words.size()], i % 3 ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD, false, scenario == 2,
                   false, 0, i * 8);
    if (scenario == 2) text.setRubyGroupAt(0, 2, "にほん");
    FsFile output;
    EXPECT_TRUE(Storage.openFileForWrite("test", "layout", output));
    GfxRenderer renderer;
    bool ok = text.layoutAndExtractLines(renderer, font, 240,
                                         [&](std::shared_ptr<TextBlock> block, uint32_t offset, uint32_t) {
                                           EXPECT_TRUE(block->valid());
                                           EXPECT_TRUE(block->serialize(output));
                                           EXPECT_EQ(output.write(&offset, sizeof(offset)), sizeof(offset));
                                         });
    output.close();
    if (result)
      *result = ok;
    else
      EXPECT_TRUE(ok);
    return Storage.bytes("layout");
  }
};
TEST_F(TextLayoutBackingTest, SameSerializedLinesAcrossPoolsFontsAndReadingAids) {
  std::ofstream reference;
  if (const char* path = std::getenv("LAYOUT_REFERENCE_OUT")) reference.open(path, std::ios::binary);
  for (int font : {0, 2, 5})
    for (int scenario = 0; scenario < 6; ++scenario) {
      fakeheap::reset(false);
      const auto expected = layout(font, scenario);
      ASSERT_FALSE(expected.empty());
      EXPECT_TRUE(fakeheap::live.empty());
      if (reference) reference.write(reinterpret_cast<const char*>(expected.data()), expected.size());
      fakeheap::reset();
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
      fakeheap::reset();
      fakeheap::external.fail = 1000;
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
    }
}
#ifndef PSRAM_REFERENCE
TEST_F(TextLayoutBackingTest, AllocationFailureReturnsWithoutPartialLinesOrLeaks) {
  fakeheap::external.fail = 1000;
  fakeheap::internal.largest = 1;
  bool ok = true;
  auto output = layout(0, 0, &ok);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(output.empty());
  EXPECT_TRUE(fakeheap::live.empty());
}
#endif

TEST_F(TextLayoutBackingTest, SizedLineSurvivesCacheRoundTrip) {
  BlockStyle style;
  style.fontSize = 32;
  style.lineHeight = 68;
  ParsedText text(false, false, false, false, false, 0, style);
  text.addWord("Heading", EpdFontFamily::BOLD, false, false, false, 0, 0);
  GfxRenderer renderer;
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "sized-line", output));
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    ASSERT_TRUE(block->serialize(output));
  }));
  output.close();
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "sized-line", input));
  auto restored = TextBlock::deserialize(input);
  input.close();
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getBlockStyle().fontSize, 32);
  EXPECT_EQ(restored->getBlockStyle().lineHeight, 68);
  ASSERT_EQ(restored->wordCount(), 1);
  EXPECT_STREQ(restored->wordText(0), "Heading");
  EXPECT_EQ(restored->wordStyle(0), EpdFontFamily::BOLD);
}

TEST_F(TextLayoutBackingTest, MixedWordSizesSurviveCacheRoundTripAcrossPools) {
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    ParsedText text(false, false, false, false, false, 0, BlockStyle{});
    text.addWord("Base", EpdFontFamily::REGULAR, false, false, false, 0, 0);
    text.addWord("Large", EpdFontFamily::BOLD, false, false, false, 0, 5, 5, 0, false, 36);
    text.addWord("Base", EpdFontFamily::REGULAR, false, false, false, 0, 11);
    GfxRenderer renderer;
    FsFile output;
    ASSERT_TRUE(Storage.openFileForWrite("test", "mixed-line", output));
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
      ASSERT_TRUE(block->serialize(output));
    }));
    output.close();
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "mixed-line", input));
    auto restored = TextBlock::deserialize(input);
    input.close();
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->wordCount(), 3);
    EXPECT_EQ(restored->wordFontSize(0), 0);
    EXPECT_EQ(restored->wordFontSize(1), 36);
    EXPECT_EQ(restored->wordFontSize(2), 0);
    EXPECT_STREQ(restored->wordText(1), "Large");
  }
}

TEST_F(TextLayoutBackingTest, DropCapLogicalWordSurvivesLayoutAndCacheForSelection) {
  ParsedText text(false, false, false, true, false, 0, BlockStyle{});
  text.setInitialLetter("H");
  text.addWord("ello", EpdFontFamily::REGULAR, false, false, false, 0, 1);
  text.addWord("world", EpdFontFamily::REGULAR, false, false, false, 0, 6);
  GfxRenderer renderer;
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "drop-cap", output));
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    EXPECT_STREQ(block->wordText(0), "Hello");
    EXPECT_STREQ(block->visibleWordText(0), "ello");
    EXPECT_EQ(block->wordTextLen(0), 5);
    EXPECT_EQ(block->visibleWordTextLen(0), 4);
    ASSERT_TRUE(block->serialize(output));
  }));
  output.close();
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "drop-cap", input));
  auto restored = TextBlock::deserialize(input);
  input.close();
  ASSERT_NE(restored, nullptr);
  EXPECT_STREQ(restored->wordText(0), "Hello");
  EXPECT_STREQ(restored->visibleWordText(0), "ello");
  EXPECT_STREQ(restored->wordText(1), "world");
}

TEST_F(TextLayoutBackingTest, MixedWordBaselinesAndScriptOffsetsMatchHighlightRedraw) {
  GfxRenderer renderer;
  renderer.scalable = true;
  TextBlock block({"small", "large", "raised"}, {0, 40, 100},
                  {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::SUP}, {}, {}, {}, {}, {},
                  BlockStyle{}, {}, {12, 24, 12});
  ASSERT_TRUE(block.valid());
  EXPECT_EQ(block.wordFontId(renderer, 0, 1), 24);
  EXPECT_EQ(block.wordYOffset(renderer, 0, 0), 12);
  EXPECT_EQ(block.wordYOffset(renderer, 0, 1), 0);
  EXPECT_EQ(block.wordYOffset(renderer, 0, 2), 12 - 24 * 2 / 5);
}

TEST_F(TextLayoutBackingTest, CharacterSpacingSurvivesCacheRoundTrip) {
  for (int8_t spacing : {-5, 0, 5}) {
    ParsedText text(false, false, false, false, false, 0, BlockStyle{}, false, spacing);
    text.addWord("Spacing", EpdFontFamily::REGULAR);
    GfxRenderer renderer;
    FsFile output;
    ASSERT_TRUE(Storage.openFileForWrite("test", "spacing-line", output));
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
      EXPECT_EQ(block->getCharacterSpacing(), spacing);
      ASSERT_TRUE(block->serialize(output));
    }));
    output.close();
    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "spacing-line", input));
    auto restored = TextBlock::deserialize(input);
    input.close();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->getCharacterSpacing(), spacing);
    EXPECT_STREQ(restored->wordText(0), "Spacing");
  }
}

TEST_F(TextLayoutBackingTest, PublisherRubyReservesItsActualEdgeOverhang) {
  GfxRenderer renderer;
  renderer.publisherFont = 30;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(false, false, false, false, false, 0, style);
  text.addWord("a", EpdFontFamily::REGULAR, false, false, false, 0, 0, 0, 0, false, 0, 123);
  text.setRubyForWordAt(0, "long");
  unsigned lines = 0;
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 180, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    ++lines;
    EXPECT_EQ(block->wordCount(), 1);
    EXPECT_EQ(block->wordFontId(renderer, 0, 0), 30);
    // Stub widths: base=36, annotation=144. Reserve half the 108px overhang on each edge.
    EXPECT_GE(block->wordXpos(0), 54);
    EXPECT_LE(block->wordXpos(0) + 36 + 54, 180);
  }));
  EXPECT_EQ(lines, 1u);
}

TEST_F(TextLayoutBackingTest, PublisherBaselineIsComputedOncePerRender) {
  GfxRenderer renderer;
  renderer.publisherFont = 3;
  constexpr size_t count = 30;
  TextBlock block(std::vector<std::string>(count, "a"), std::vector<int16_t>(count, 0),
                  std::vector<EpdFontFamily::Style>(count, EpdFontFamily::REGULAR), {}, {}, {}, {}, {}, BlockStyle{},
                  {}, {}, "", 0, std::vector<uint32_t>(count, 123));
  ASSERT_TRUE(block.valid());
  block.render(renderer, 0, 0, 0);
  EXPECT_LE(renderer.ascenderCalls, count * 2 + 1);
}

TEST_F(TextLayoutBackingTest, SourceRangesSurviveCacheRoundTrip) {
  ParsedText text(false);
  text.addWord("same", EpdFontFamily::REGULAR, false, false, false, 0, 20);
  text.addWord("phrase", EpdFontFamily::REGULAR, false, false, false, 0, 25);
  GfxRenderer renderer;
  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "source-line", output));
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 480, [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) {
    ASSERT_TRUE(line->serialize(output));
  }));
  output.close();
  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "source-line", input));
  auto restored = TextBlock::deserialize(input);
  input.close();
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), 2);
  EXPECT_EQ(restored->wordSourceRange(0).start, 20U);
  EXPECT_EQ(restored->wordSourceRange(0).end, 24U);
  EXPECT_EQ(restored->wordSourceRange(1).start, 25U);
  EXPECT_EQ(restored->wordSourceRange(1).end, 31U);
}
