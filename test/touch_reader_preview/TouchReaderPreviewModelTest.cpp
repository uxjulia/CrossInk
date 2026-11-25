#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "TouchReaderPreviewModel.h"

namespace {

std::shared_ptr<TextBlock> makeLine(std::initializer_list<const char*> words, const int16_t wordStep = 4,
                                    const bool firstHasSpaceBefore = false) {
  std::vector<TextBlock::Word> line;
  int16_t x = 0;
  for (const char* word : words) {
    TextBlock::Word entry;
    entry.text = word;
    entry.x = x;
    entry.hasSpaceBefore = firstHasSpaceBefore || !line.empty();
    line.push_back(std::move(entry));
    x = static_cast<int16_t>(x + wordStep);
  }
  return std::make_shared<TextBlock>(std::move(line));
}

}  // namespace

TEST(TouchReaderPreviewModel, ReflowsAcrossCapturedLineBreaksBeforeJustifying) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}), 0, 0));
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"cc", "dd"}, 4, true), 0, 10));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 16, 100, 0, static_cast<uint8_t>(CssTextAlign::Justify), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].text, "aa");
  EXPECT_EQ(renderer.drawCalls[0].x, 0);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 6);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].text, "cc");
  EXPECT_EQ(renderer.drawCalls[2].x, 12);
  EXPECT_EQ(renderer.drawCalls[2].y, 0);
  EXPECT_EQ(renderer.drawCalls[3].text, "dd");
  EXPECT_EQ(renderer.drawCalls[3].y, 10);
}

TEST(TouchReaderPreviewModel, BalancesLineBreaksLikeTheReaderLayout) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "b", "c", "ddd"}), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 7, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].y, 10);
  EXPECT_EQ(renderer.drawCalls[2].y, 10);
  EXPECT_EQ(renderer.drawCalls[3].y, 20);
}

TEST(TouchReaderPreviewModel, WordSpacingReflowsPreviewText) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb", "cc", "dd"}), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 20, 100, 1, static_cast<uint8_t>(CssTextAlign::Justify), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].y, 10);
  EXPECT_EQ(renderer.drawCalls[3].y, 10);
}

TEST(TouchReaderPreviewModel, NormalWordSpacingRemovesTheAdditionalPreviewGap) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 2, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  const int spacedX = renderer.drawCalls[1].x;
  EXPECT_EQ(spacedX, 25);

  renderer.drawCalls.clear();
  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].x, 5);
}

TEST(TouchReaderPreviewModel, WordSpacingUsesSourceWhitespaceInsteadOfPixelGaps) {
  Page page;
  // The source's Focus/SD-font metrics can put this word at the same x
  // position a plain whole-word advance would predict. The whitespace bit is
  // still authoritative and must survive into the preview.
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}, 2), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 30, 100, 1, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 15);
}

TEST(TouchReaderPreviewModel, NormalWordSpacingRecoversVisibleSourceSpacesWhenMetadataIsMissing) {
  Page page;
  std::vector<TextBlock::Word> source = {{"aa", 0}, {"bb", 23}};
  source[1].hasSpaceBefore = false;
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(source)), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 5);
}

TEST(TouchReaderPreviewModel, PreservesAttachedSourceTokens) {
  Page page;
  std::vector<TextBlock::Word> attached = {{"can", 0}, {"not", 3}};
  attached[1].hasSpaceBefore = false;
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(attached)), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 30, 100, 1, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "not");
  EXPECT_EQ(renderer.drawCalls[1].x, 6);
}

TEST(TouchReaderPreviewModel, PreservesNoSpaceCjkBreakOpportunities) {
  Page page;
  std::vector<TextBlock::Word> cjk = {{"你", 0}, {"好", 3}, {"啊", 6}};
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(cjk)), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 12, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 3U);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].y, 10);
}

TEST(TouchReaderPreviewModel, RejoinsLayoutInsertedHyphenWithinCurrentPage) {
  Page page;
  std::vector<TextBlock::Word> prefix = {{"exam-", 0}};
  prefix[0].endsWithInsertedHyphen = true;
  std::vector<TextBlock::Word> suffix = {{"ple", 0}};
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(prefix)), 0, 0));
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(suffix)), 0, 10));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 30, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[0].text, "exam");
  EXPECT_EQ(renderer.drawCalls[1].text, "ple");
  EXPECT_EQ(renderer.drawCalls[1].x, 8);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
}

TEST(TouchReaderPreviewModel, RestoresLayoutInsertedHyphenWhenPreviewStillBreaksThere) {
  Page page;
  std::vector<TextBlock::Word> prefix = {{"exam-", 0}};
  prefix[0].endsWithInsertedHyphen = true;
  std::vector<TextBlock::Word> suffix = {{"ple", 0}};
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(prefix)), 0, 0));
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(suffix)), 0, 10));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 9, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 3U);
  EXPECT_EQ(renderer.drawCalls[0].text, "exam");
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].text, "-");
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].text, "ple");
  EXPECT_EQ(renderer.drawCalls[2].y, 10);
}

TEST(TouchReaderPreviewModel, PreservesHalfLineParagraphSpacing) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"first"}), 0, 0));
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"second"}), 0, 15));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].y, 15);
}

TEST(TouchReaderPreviewModel, DoesNotInventAnIndentFromSourcePixelPosition) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"first"}), 12, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 1U);
  EXPECT_EQ(renderer.drawCalls[0].x, 0);
}

TEST(TouchReaderPreviewModel, DoesNotIndentPageStartParagraphContinuation) {
  Page page;
  BlockStyle style;
  style.textIndentDefined = true;
  style.textIndent = 6;
  page.elements.push_back(std::make_unique<PageLine>(
      std::make_shared<TextBlock>(std::vector<TextBlock::Word>{{"middle", 0}}, style), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 1U);
  EXPECT_EQ(renderer.drawCalls[0].x, 0);
}

TEST(TouchReaderPreviewModel, SourceLayoutSurvivesPageReleaseAndPreviewReflow) {
  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  {
    Page page;
    page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}, 17), 13, 7));
    page.elements.push_back(std::make_unique<PageLine>(makeLine({"cc", "dd"}, 21), 3, 24));
    ASSERT_TRUE(model.capture(page, renderer, 1, 100, 11, 19));
  }
  model.renderText(renderer, 2, 0, 0, 16, 100, 0, 0, false, false, true);
  renderer.drawCalls.clear();
  model.renderSource(renderer, 1, true);
  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].x, 24);
  EXPECT_EQ(renderer.drawCalls[0].y, 26);
  EXPECT_EQ(renderer.drawCalls[1].x, 41);
  EXPECT_EQ(renderer.drawCalls[2].x, 14);
  EXPECT_EQ(renderer.drawCalls[2].y, 43);
  EXPECT_EQ(renderer.drawCalls[3].x, 35);
}

TEST(TouchReaderPreviewModel, NextCaptureReleasesPreviousSourceBlocks) {
  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  std::weak_ptr<TextBlock> previous;
  {
    Page page;
    auto block = makeLine({"aa", "bb"});
    previous = block;
    page.elements.push_back(std::make_unique<PageLine>(block, 0, 0));
    ASSERT_TRUE(model.capture(page, renderer, 1, 100));
  }
  EXPECT_FALSE(previous.expired());
  Page empty;
  EXPECT_FALSE(model.capture(empty, renderer, 1, 100));
  EXPECT_TRUE(previous.expired());
  model.renderSource(renderer, 1, true);
  EXPECT_TRUE(renderer.drawCalls.empty());
}

TEST(TouchReaderPreviewModel, KeepsBoundedPreviewWhenPageHasMoreLinesThanSnapshot) {
  Page page;
  for (size_t i = 0; i < TouchReaderPreviewModel::LINE_CAPACITY + 1; ++i) {
    page.elements.push_back(std::make_unique<PageLine>(makeLine({"word"}), 0, static_cast<int16_t>(i * 10)));
  }

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Right), false, false, true);
  EXPECT_EQ(renderer.drawCalls.size(), TouchReaderPreviewModel::LINE_CAPACITY);
  EXPECT_GT(renderer.drawCalls.back().x, 0);
}

TEST(SampleReaderPreviewModel, ContainsTheWholeParagraphWithoutABookPage) {
  SampleReaderPreviewModel model;
  ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
  GfxRenderer renderer;
  model.renderText(renderer, 1, 5, 8, 100, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  std::string paragraph;
  for (const auto& call : renderer.drawCalls) {
    if (!paragraph.empty()) paragraph += ' ';
    paragraph += call.text;
    EXPECT_GE(call.x, 5);
  }
  EXPECT_EQ(paragraph, READER_PREVIEW_PARAGRAPH);
  EXPECT_GT(renderer.drawCalls.back().y, renderer.drawCalls.front().y);
  EXPECT_LE(sizeof(model), 3U * 1024U);
}

TEST(SampleReaderPreviewModel, ReflowsForMarginsFontSizeAndSpacing) {
  SampleReaderPreviewModel model;
  ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
  const auto lastY = [&](int font, int width, int lineSpacing, int wordSpacing) {
    GfxRenderer renderer;
    model.renderText(renderer, font, 0, 0, width, lineSpacing, wordSpacing, static_cast<uint8_t>(CssTextAlign::Left),
                     false, false, true);
    return renderer.drawCalls.back().y;
  };
  const int normal = lastY(1, 100, 100, 0);
  EXPECT_GT(lastY(1, 60, 100, 0), normal);
  EXPECT_GT(lastY(2, 100, 100, 0), normal);
  EXPECT_GT(lastY(1, 100, 150, 0), normal);
  EXPECT_GT(lastY(1, 100, 100, 4), normal);
}

TEST(SampleReaderPreviewModel, SupportsAlignmentFocusAndGuideDots) {
  SampleReaderPreviewModel model;
  ASSERT_TRUE(model.captureParagraph("Lorem ipsum dolor sit amet."));
  GfxRenderer left, right, decorated;
  model.renderText(left, 1, 0, 0, 100, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  model.renderText(right, 1, 0, 0, 100, 100, 0, static_cast<uint8_t>(CssTextAlign::Right), false, false, true);
  model.renderText(decorated, 1, 0, 0, 100, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), true, true, true);
  EXPECT_GT(right.drawCalls.front().x, left.drawCalls.front().x);
  bool bold = false, dot = false;
  for (const auto& call : decorated.drawCalls) {
    bold = bold || (call.style & EpdFontFamily::BOLD);
    dot = dot || call.text == "·";
  }
  EXPECT_TRUE(bold);
  EXPECT_TRUE(dot);
}

TEST(SampleReaderPreviewModel, RejectsOverlongSamplesWithoutLeavingAStalePreview) {
  ReaderPreviewModel<32, 2, 1, false> model;
  ASSERT_TRUE(model.captureParagraph("one two"));
  EXPECT_FALSE(model.captureParagraph("one two three"));
  EXPECT_FALSE(model.valid());
  EXPECT_FALSE(model.captureParagraph("This sample exceeds the available text capacity."));
  EXPECT_FALSE(model.valid());
  EXPECT_FALSE(model.captureParagraph("   "));
  EXPECT_FALSE(model.valid());
}

TEST(SampleReaderPreviewModel, DrawsOnlyCompleteLinesInsideThePreview) {
  SampleReaderPreviewModel model;
  ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
  for (const int spacing : {70, 100, 200}) {
    GfxRenderer renderer;
    model.renderText(renderer, 1, 0, 5, 80, spacing, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true,
                     24);
    ASSERT_FALSE(renderer.drawCalls.empty());
    for (const auto& call : renderer.drawCalls) EXPECT_LE(call.y + renderer.getTextHeight(1), 24);
  }
  GfxRenderer tiny;
  model.renderText(tiny, 1, 0, 5, 80, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true, 10);
  EXPECT_TRUE(tiny.drawCalls.empty());
}

TEST(TouchReaderPreviewModel, NegativeWordSpacingTightensNaturalGaps) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}), 0, 0));
  GfxRenderer renderer;
  renderer.spaceWidth = 5;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));
  for (bool guide : {false, true}) {
    int previous = 1000;
    int original = 0;
    for (int level = 0; level >= -4; --level) {
      renderer.drawCalls.clear();
      model.renderText(renderer, 2, 0, 0, 100, 100, WordSpacing::fromLevel(level),
                       static_cast<uint8_t>(CssTextAlign::Left), false, guide, true);
      ASSERT_EQ(renderer.drawCalls.size(), guide ? 3u : 2u);
      const int x = renderer.drawCalls.back().x;
      EXPECT_LE(x, previous);
      EXPECT_GT(x, 4);
      if (level == 0) original = x;
      previous = x;
    }
    EXPECT_EQ(previous, original - 4);
  }
}

TEST(WordSpacing, SavedValuesAndSliderRoundTrip) {
  for (int level = -4; level <= 4; ++level) {
    const auto value = WordSpacing::fromLevel(level);
    EXPECT_EQ(WordSpacing::level(value), level);
    EXPECT_EQ(WordSpacing::fromSlider(WordSpacing::sliderValue(value)), value);
    if (level >= 0) {
      EXPECT_EQ(value, level);
      EXPECT_EQ(WordSpacing::extra(6, value), 10 * level);
    }
  }
  EXPECT_EQ(WordSpacing::fromSlider(4), 0);
  for (int gap = 1; gap <= 30; ++gap) {
    int previous = gap;
    for (int level = -1; level >= -4; --level) {
      const int adjusted = gap + WordSpacing::extra(gap, WordSpacing::fromLevel(level));
      EXPECT_GE(adjusted, 1);
      EXPECT_LE(adjusted, previous);
      previous = adjusted;
    }
  }
}
