#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

TEST(ParagraphIndentTest, DoesNotInventIndentWithoutSourceCss) {
  GfxRenderer renderer;

  for (const bool extraParagraphSpacing : {false, true}) {
    ParsedText paragraph(extraParagraphSpacing);
    EXPECT_EQ(paragraph.resolveFirstLineIndent(true, renderer, 0), 0);
  }
}

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  Epub epub;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{epub,  filepath, renderer, 0,  1.0f, false, false, 0, 480, 800,     false,
                               false, false,    0,        {}, true, "",    "",    0, {},  nullptr, &cssParser};
  std::array<ChapterHtmlSlimParser::StyleStackEntry, 4> inlineStyles{};
  std::array<BlockStyle, 4> blockStyles{};

  void SetUp() override {
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
    parser.inlineStyleBuf_ = inlineStyles.data();
    parser.blockStyleBuf_ = blockStyles.data();
    parser.blockStyleCount_ = 1;
  }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_NE(footnote.linkId, 0u);
  ASSERT_EQ(parser.currentTextBlock->wordBackgroundBlack.size(), 1u);
  const uint8_t wordLinkId =
      static_cast<uint8_t>((parser.currentTextBlock->wordBackgroundBlack.front() & TextBlock::WORD_FLAG_LINK_ID_MASK) >>
                           TextBlock::WORD_FLAG_LINK_ID_SHIFT);
  EXPECT_EQ(wordLinkId, footnote.linkId);
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, UsesOptimizerImageDimensionsWithoutReadingTheCompressedImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 800;
  epub.optimizerImageHeight = 7;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  EXPECT_EQ(epub.streamReadCount, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 4);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenElementsSuppressContentAndResumeVisibleText) {
  for (const char* tag : {"p", "h1", "span", "div", "a", "table"}) {
    for (const char* value : {"hidden", "", "false"}) {
      const XML_Char* attributes[] = {"hidden", value, "style", "display: block", nullptr};
      ChapterHtmlSlimParser::startElement(&parser, tag, attributes);
      ChapterHtmlSlimParser::startElement(&parser, "span", nullptr);
      ChapterHtmlSlimParser::characterData(&parser, "HIDDEN ", 7);
      ChapterHtmlSlimParser::endElement(&parser, "span");
      ChapterHtmlSlimParser::endElement(&parser, tag);
      EXPECT_EQ(parser.currentTextBlock->size(), 0u);
      EXPECT_EQ(parser.partWordBufferIndex, 0);
    }
  }
  ChapterHtmlSlimParser::characterData(&parser, "Visible ", 8);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Visible");
}

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsCollapseClusteredWhitespace) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  constexpr char text[] = "  Alpha     Beta ";
  ChapterHtmlSlimParser::characterData(&parser, text, sizeof(text) - 1);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 6u);
  EXPECT_EQ(parser.referenceTextOffset, 10u);
  EXPECT_TRUE(parser.referenceWhitespacePending);
}

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsResumeAfterNestedExcludedMarkup) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  ChapterHtmlSlimParser::startElement(&parser, "html", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "head", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "style", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "p { display: block; }", 21);
  ChapterHtmlSlimParser::endElement(&parser, "style");
  ChapterHtmlSlimParser::endElement(&parser, "head");
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "svg", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "metadata", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "not book text", 13);
  ChapterHtmlSlimParser::endElement(&parser, "metadata");
  ChapterHtmlSlimParser::endElement(&parser, "svg");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Visible text ", 13);
  parser.flushPartWordBuffer();

  EXPECT_EQ(parser.referenceExcludedUntilDepth, INT_MAX);
  EXPECT_EQ(parser.referenceTextOffset, 12u);
  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 8u);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenImageDoesNotReadImageDataWithoutCss) {
  parser.cssParser = nullptr;
  const XML_Char* attributes[] = {"hidden", "", "src", "missing.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(parser.currentPage, nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenIdsDoNotBecomeAnchorsOrTocPageBreaks) {
  parser.tocAnchors.push_back("hidden-chapter");
  const XML_Char* idFirst[] = {"id", "hidden-chapter", "hidden", "hidden", nullptr};
  const XML_Char* hiddenFirst[] = {"hidden", "", "id", "hidden-chapter", nullptr};
  for (auto* attributes : {idFirst, hiddenFirst}) {
    ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
    ChapterHtmlSlimParser::characterData(&parser, "Hidden", 6);
    ChapterHtmlSlimParser::endElement(&parser, "h1");
    EXPECT_TRUE(parser.pendingAnchorId.empty());
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_TRUE(parser.anchorData.empty());
    EXPECT_EQ(parser.completedPageCount, 0);
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
}

TEST_F(ChapterHtmlSlimParserTest, NumbersOrderedListsAndRestartsNestedCounters) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");

  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ol");

  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

TEST_F(ChapterHtmlSlimParserTest, HonorsOrderedListStartAndItemValue) {
  const XML_Char* listAttributes[] = {"start", "5", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "5.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* itemAttributes[] = {"value", "9", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "li", itemAttributes);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "9.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "10.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsNegativeOrderedListValues) {
  const XML_Char* listAttributes[] = {"start", "-2", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-2.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-1.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsMarkerFreeListsAndContainerInsets) {
  const XML_Char* listAttributes[] = {"style", "list-style-type: none; margin-left: 10px; padding-left: 5px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);

  EXPECT_TRUE(parser.currentTextBlock->isEmpty());
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 15);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenNestedListDoesNotResetOuterCounter) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* hidden[] = {"hidden", "", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", hidden);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

}  // namespace
