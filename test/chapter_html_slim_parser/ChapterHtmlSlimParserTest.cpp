#include <Epub/Page.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <Epub.h>
#include <MemoryBudget.h>

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
  std::array<ChapterHtmlSlimParser::StyleStackEntry, ChapterHtmlSlimParser::MAX_INLINE_STYLE_DEPTH> inlineStyles{};
  std::array<BlockStyle, ChapterHtmlSlimParser::MAX_BLOCK_STYLE_DEPTH> blockStyles{};

  void SetUp() override {
    GfxRenderer::loanActive = false;
    GfxRenderer::fileProbeHadLoan = false;
    MemoryBudget::imageAllowed = true;
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
    parser.inlineStyleBuf_ = inlineStyles.data();
    parser.blockStyleBuf_ = blockStyles.data();
    parser.blockStyleCount_ = 1;
  }

  void expectWordAcrossLayoutThreshold(const std::string& first, const std::string& second) {
    // Stay just below the byte limit with complete words, then cross it mid-word.
    const size_t prefixBytes = parser.textRunBytesBeforeLayoutLimit() - 1;
    std::string prefix;
    for (size_t remaining = prefixBytes; remaining > 0;) {
      const size_t count = std::min(remaining, size_t{100});
      prefix += std::string(count, 'x') + " ";
      remaining -= count;
    }
    ChapterHtmlSlimParser::characterData(&parser, prefix.data(), prefix.size());
    ASSERT_EQ(parser.currentTextRunBytes, prefixBytes);
    ChapterHtmlSlimParser::characterData(&parser, first.data(), first.size());
    EXPECT_EQ(std::string(parser.partWordBuffer, parser.partWordBufferIndex), first);
    const std::string tail = second + " ";
    ChapterHtmlSlimParser::characterData(&parser, tail.data(), tail.size());
    const auto& words = parser.currentTextBlock->words;
    EXPECT_EQ(std::count(words.begin(), words.end(), first + second), 1);
    EXPECT_EQ(std::count(words.begin(), words.end(), first), 0);
    EXPECT_EQ(std::count(words.begin(), words.end(), second), 0);
    EXPECT_EQ(parser.partWordBufferIndex, 0);
    EXPECT_FALSE(parser.lowMemoryAbort);
  }

  void expectChapterFigureLayout(CssTextAlign alignment, uint16_t imageHeight = 6, bool blockDisplay = false) {
    parser.paragraphAlignment = static_cast<uint8_t>(alignment);
    cssParser.rulesBySelector_["figure.inline_group"] =
        CssParser::parseInlineStyle("margin-top: 0.5em; margin-bottom: 3em; text-align: center");
    epub.optimizerImageAvailable = true;
    epub.optimizerImageWidth = 115;
    epub.optimizerImageHeight = imageHeight;
    const XML_Char* figureAttributes[] = {"class", "inline_group", nullptr};
    const XML_Char* imageAttributes[] = {"src", "ornament.jpg", "style", blockDisplay ? "display: block" : "", nullptr};

    ChapterHtmlSlimParser::startElement(&parser, "h1", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "1", 1);
    ChapterHtmlSlimParser::endElement(&parser, "h1");
    ChapterHtmlSlimParser::startElement(&parser, "figure", figureAttributes);
    const auto figureStyle = parser.currentTextBlock->getBlockStyle();
    EXPECT_EQ(figureStyle.alignment, CssTextAlign::Center);
    EXPECT_EQ(figureStyle.marginTop, 6);
    EXPECT_EQ(figureStyle.marginBottom, 0);  // Applied when the figure closes.
    ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    ChapterHtmlSlimParser::endElement(&parser, "figure");
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment,
              alignment == CssTextAlign::None ? CssTextAlign::Justify : alignment);
    ChapterHtmlSlimParser::characterData(&parser, "Date", 4);
    parser.flushPartWordBuffer();
    parser.makePages();

    ASSERT_NE(parser.currentPage, nullptr);
    ASSERT_EQ(parser.currentPage->elements.size(), 3u);
    ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
    ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
    ASSERT_EQ(parser.currentPage->elements[2]->getTag(), TAG_PageLine);
    const auto& heading = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
    const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements[1]);
    const auto& following = static_cast<const PageLine&>(*parser.currentPage->elements[2]);
    EXPECT_EQ(heading.getBlock()->wordCount(), 1u);
    EXPECT_EQ(image.xPos, (parser.viewportWidth - 115) / 2);
    EXPECT_GE(image.yPos, heading.yPos + renderer.getLineHeight(0) + 6);
    EXPECT_EQ(following.yPos, image.yPos + imageHeight + 36);
    EXPECT_EQ(parser.currentPageNextY, following.yPos + renderer.getLineHeight(0));
    EXPECT_EQ(following.getBlock()->wordCount(), 1u);
    EXPECT_TRUE(parser.pendingInlineImages.empty());
    EXPECT_EQ(parser.blockStyleCount_, 2u);  // Root and following paragraph only.
  }

  void expectNestedFigureClosingSpacing() {
    cssParser.rulesBySelector_["div"] = CssParser::parseInlineStyle("margin-top: 10px; margin-bottom: 12px");
    cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
    epub.optimizerImageAvailable = true;
    epub.optimizerImageWidth = 115;
    epub.optimizerImageHeight = 60;
    const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
    ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    ChapterHtmlSlimParser::endElement(&parser, "figure");
    ChapterHtmlSlimParser::endElement(&parser, "div");
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "After", 5);
    parser.flushPartWordBuffer();
    parser.makePages();
    ASSERT_NE(parser.currentPage, nullptr);
    ASSERT_EQ(parser.currentPage->elements.size(), 2u);
    EXPECT_EQ(parser.currentPage->elements[0]->yPos, 10);
    EXPECT_EQ(parser.currentPage->elements[1]->yPos, 118);
    EXPECT_EQ(parser.currentPageNextY, 118 + renderer.getLineHeight(0));
  }

  BlockStyle parseParagraph(const char* className) {
    const XML_Char* attributes[] = {"class", className, nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
    const BlockStyle style = parser.currentTextBlock->getBlockStyle();
    ChapterHtmlSlimParser::characterData(&parser, "Text", 4);
    ChapterHtmlSlimParser::endElement(&parser, "p");
    return style;
  }
};

// Regression cases adapted from yassine-cc's fix for CrossInk #886.
TEST_F(ChapterHtmlSlimParserTest, NestedQuotationKeepsPrecedingWordOutsideQuote) {
  cssParser.rulesBySelector_[".quote"] = CssParser::parseInlineStyle("margin-left: 3em");
  const XML_Char* quoteAttributes[] = {"class", "quote", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
  const std::string introduction = "Their hypothesis was remarkably straightforward:";
  ChapterHtmlSlimParser::characterData(&parser, introduction.data(), introduction.size());
  ChapterHtmlSlimParser::startElement(&parser, "div", quoteAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "i", nullptr);
  const std::string quotation = "If an MBA education";
  ChapterHtmlSlimParser::characterData(&parser, quotation.data(), quotation.size());
  ChapterHtmlSlimParser::endElement(&parser, "i");

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  const auto& introductionLine = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(introductionLine.getBlock()->wordCount(), 5u);
  EXPECT_EQ(introductionLine.xPos, 0);
  EXPECT_EQ(introductionLine.yPos, 0);

  ASSERT_EQ(parser.currentTextBlock->size(), 4u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "If");
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::ITALIC);
  EXPECT_FALSE(parser.currentTextBlock->wordContinues[0]);
  const int16_t quoteInset = parser.currentTextBlock->getBlockStyle().leftInset();
  EXPECT_GT(quoteInset, 0);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::endElement(&parser, "div");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, quoteInset);
  EXPECT_GT(parser.currentPage->elements[1]->yPos, introductionLine.yPos);
}

TEST_F(ChapterHtmlSlimParserTest, NestedHeadingDoesNotAbsorbSingleBufferedWord) {
  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::startElement(&parser, "h2", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Heading", 7);
  ChapterHtmlSlimParser::endElement(&parser, "h2");
  ChapterHtmlSlimParser::endElement(&parser, "div");
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  for (const auto& element : parser.currentPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    EXPECT_EQ(static_cast<const PageLine&>(*element).getBlock()->wordCount(), 1u);
  }
}

TEST_F(ChapterHtmlSlimParserTest, BlockquoteParagraphsInheritItalicAndRestoreFollowingText) {
  cssParser.rulesBySelector_["blockquote"] =
      CssParser::parseInlineStyle("margin-left: 2em; margin-right: 1em; font-style: italic");
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", nullptr);

  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "Quoted", 6);
    // Closing the paragraph must flush its final word before changing styles.
    ChapterHtmlSlimParser::endElement(&parser, "p");
    ASSERT_EQ(parser.currentTextBlock->size(), 1u);
    EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::ITALIC);
    EXPECT_TRUE(parser.effectiveItalic);
    EXPECT_EQ(parser.inlineStyleCount_, 1u);
  }

  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
  EXPECT_FALSE(parser.effectiveItalic);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Following", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::REGULAR);
}

TEST_F(ChapterHtmlSlimParserTest, NestedBlocksOverrideAndRestoreInheritedBoldAndItalic) {
  const XML_Char* parentAttributes[] = {"style", "font-weight: bold; font-style: italic", nullptr};
  const XML_Char* normalAttributes[] = {"style", "font-weight: normal; font-style: normal", nullptr};
  const XML_Char* italicAttributes[] = {"style", "font-style: italic", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parentAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", normalAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Normal ", 7);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::REGULAR);

  ChapterHtmlSlimParser::startElement(&parser, "span", italicAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Italic", 6);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::ITALIC);
  ChapterHtmlSlimParser::characterData(&parser, "Normal", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(2), EpdFontFamily::REGULAR);

  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Inherited", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));
  ChapterHtmlSlimParser::endElement(&parser, "div");
  EXPECT_FALSE(parser.effectiveBold);
  EXPECT_FALSE(parser.effectiveItalic);
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, NestedHeadingAndListPreserveBlockFontInheritance) {
  const XML_Char* attributes[] = {"style", "font-style: italic", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "h2", nullptr);
  EXPECT_TRUE(parser.effectiveItalic);
  ChapterHtmlSlimParser::characterData(&parser, "Heading", 7);
  ChapterHtmlSlimParser::endElement(&parser, "h2");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));

  ChapterHtmlSlimParser::startElement(&parser, "ul", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Item", 4);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::ITALIC);
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");
  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  EXPECT_FALSE(parser.effectiveItalic);
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, TableCellBlocksInheritOverrideAndRestoreFontStyles) {
  const XML_Char* parentAttributes[] = {"style", "font-weight: bold; font-style: italic", nullptr};
  const XML_Char* normalAttributes[] = {"style", "font-weight: normal; font-style: normal", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", parentAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Quoted", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));

  ChapterHtmlSlimParser::startElement(&parser, "p", normalAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Normal", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::REGULAR);
  EXPECT_TRUE(parser.effectiveBold);
  EXPECT_TRUE(parser.effectiveItalic);

  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Following", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(2), EpdFontFamily::REGULAR);
  ChapterHtmlSlimParser::endElement(&parser, "td");
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
  ChapterHtmlSlimParser::endElement(&parser, "tr");
  ChapterHtmlSlimParser::endElement(&parser, "table");
}

TEST_F(ChapterHtmlSlimParserTest, InheritsBodyTextIndentAndPreservesExplicitParagraphZero) {
  parser.cssParser->rulesBySelector_[".class-0"] =
      CssParser::parseInlineStyle("text-indent: 1.5em; text-align: justify");
  parser.cssParser->rulesBySelector_[".class_s4K-0"] = CssParser::parseInlineStyle("text-indent: 0");
  parser.cssParser->rulesBySelector_[".class_s4P-0"] = CssParser::parseInlineStyle("margin-top: 0; margin-bottom: 0");

  const XML_Char* bodyAttributes[] = {"class", "class-0", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle openingParagraph = parseParagraph("class_s4K-0");
  EXPECT_TRUE(openingParagraph.textIndentDefined);
  EXPECT_EQ(openingParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  const BlockStyle followingParagraph = parseParagraph("class_s4P-0");
  EXPECT_TRUE(followingParagraph.textIndentDefined);
  EXPECT_EQ(followingParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, HtmlIndentFlowsThroughBodyAndBodyIndentOverridesIt) {
  parser.cssParser->rulesBySelector_[".html-indent"] = CssParser::parseInlineStyle("text-indent: 1em");
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");

  const XML_Char* htmlAttributes[] = {"class", "html-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "html", htmlAttributes);
  EXPECT_EQ(parser.blockStyleBuf_[0].textIndent, 12);

  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
  ChapterHtmlSlimParser::endElement(&parser, "html");
}

TEST_F(ChapterHtmlSlimParserTest, InheritsTextIndentFromDivAndKeepsParagraphOverride) {
  parser.cssParser->rulesBySelector_[".ancestor-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");

  const XML_Char* divAttributes[] = {"class", "ancestor-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", divAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  const BlockStyle zeroParagraph = parseParagraph("zero");
  EXPECT_TRUE(zeroParagraph.textIndentDefined);
  EXPECT_EQ(zeroParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "div");
}

TEST_F(ChapterHtmlSlimParserTest, BodyIndentRespectsSpacingAndForcedIndentSettings) {
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  parser.extraParagraphSpacing = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(true, false);
  const BlockStyle spacedParagraph = parseParagraph("plain");
  EXPECT_EQ(spacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  parser.extraParagraphSpacing = false;
  const BlockStyle unspacedParagraph = parseParagraph("plain");
  EXPECT_EQ(unspacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  parser.forceParagraphIndents = true;
  const BlockStyle forcedZeroParagraph = parseParagraph("zero");
  EXPECT_EQ(forcedZeroParagraph.textIndent, renderer.getFontAscenderSize(0));
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), renderer.getFontAscenderSize(0));

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, RootIndentIsIgnoredWhenEmbeddedStyleIsOff) {
  parser.embeddedStyle = false;
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle plainParagraph = parseParagraph("plain");
  EXPECT_FALSE(plainParagraph.textIndentDefined);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

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

TEST_F(ChapterHtmlSlimParserTest, PreservesEmptyInlinePaddingBeforeDialogueText) {
  parser.cssParser->rulesBySelector_[".spacey"] = CssParser::parseInlineStyle("padding-left: 2em");
  ChapterHtmlSlimParser::characterData(&parser, "EERO:", 5);
  const XML_Char* attributes[] = {"class", "spacey", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "Kappusiwai!", 11);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->words.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "EERO:");
  EXPECT_EQ(parser.currentTextBlock->words[1], "Kappusiwai!");
  ASSERT_EQ(parser.currentTextBlock->inlinePaddings.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].wordIndex, 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].pixels, 24);
  EXPECT_TRUE(parser.currentTextBlock->wordContinues[1]);

  std::shared_ptr<TextBlock> renderedLine;
  ASSERT_TRUE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 480,
      [&renderedLine](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { renderedLine = std::move(line); }));
  ASSERT_NE(renderedLine, nullptr);
  ASSERT_EQ(renderedLine->wordCount(), 2u);
  EXPECT_EQ(renderedLine->wordXpos(0), 0);
  EXPECT_EQ(renderedLine->wordXpos(1), 24);
}

// SOF header of a 800 x 200 JPEG. The real streaming dimension parser is linked.
static const std::vector<uint8_t> jpegHeader = {0xff, 0xd8, 0xff, 0xc0, 0, 7, 8, 0, 200, 3, 32};

TEST_F(ChapterHtmlSlimParserTest, OrdinaryImageHeaderDoesNotLoanWhenHeapProbeSucceeds) {
  epub.probeBytes = jpegHeader;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 1u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, RetriesImageHeaderWithLoanAndKeepsLazySource) {
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 2u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 1u);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 120);
}

TEST_F(ChapterHtmlSlimParserTest, FailedOrMalformedProbeLoansExtractionAndReturnsBuffer) {
  for (const bool streamFails : {false, true}) {
    epub.probeBytes = streamFails ? jpegHeader : std::vector<uint8_t>{0xff, 0xd8, 0x00};
    epub.streamFails = streamFails;
    epub.streamReadCount = epub.extractCount = 0;
    renderer.loans = 0;
    const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_EQ(epub.streamReadCount, 2u);
    EXPECT_EQ(epub.extractCount, 1u);
    EXPECT_TRUE(epub.extractHadLoan);
    EXPECT_EQ(renderer.loans, 2u);
    EXPECT_TRUE(renderer.hasFrameBuffer());
    EXPECT_TRUE(!parser.currentPage || parser.currentPage->elements.empty());
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
}

TEST_F(ChapterHtmlSlimParserTest, FullFileFallbackReturnsLoanBeforeDecoder) {
  epub.probeBytes = {0xff, 0xd8, 0};
  epub.extractSucceeds = true;
  const XML_Char* attributes[] = {"src", "unusual.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.extractCount, 1u);
  EXPECT_TRUE(epub.extractHadLoan);
  EXPECT_FALSE(GfxRenderer::fileProbeHadLoan);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, ImageAdmissionStillRejectsBeforeAnyProbeOrLoan) {
  MemoryBudget::imageAllowed = false;
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  EXPECT_TRUE(parser.lowMemoryImageFallback);
}

TEST_F(ChapterHtmlSlimParserTest, NestedProbeLoanDoesNotReturnOuterStorage) {
  epub.probeBytes = {0xff, 0xd8, 0};
  const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
  {
    GfxRenderer::FrameBufferLoan outer(renderer);
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_FALSE(renderer.hasFrameBuffer());
    EXPECT_EQ(renderer.loans, 1u);
  }
  EXPECT_TRUE(renderer.hasFrameBuffer());
}

TEST_F(ChapterHtmlSlimParserTest, UsesOptimizerImageDimensionsWithoutReadingTheCompressedImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 800;
  epub.optimizerImageHeight = 7;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 4);
  EXPECT_FALSE(static_cast<const PageImage&>(*parser.currentPage->elements.front()).isInlineImage());
}

TEST_F(ChapterHtmlSlimParserTest, CentersChapterFigureWithJustifiedParagraphs) {
  expectChapterFigureLayout(CssTextAlign::Justify);
}

TEST_F(ChapterHtmlSlimParserTest, CentersChapterFigureWithLeftAlignedParagraphs) {
  expectChapterFigureLayout(CssTextAlign::Left);
}

TEST_F(ChapterHtmlSlimParserTest, CentersChapterFigureWithScalableFontsAndBookStyle) {
  renderer.scalableBaseSize = 12;
  expectChapterFigureLayout(CssTextAlign::None);
}

TEST_F(ChapterHtmlSlimParserTest, BlockSizedFigureImageConsumesMarginsOnce) {
  expectChapterFigureLayout(CssTextAlign::Justify, 60);
}

TEST_F(ChapterHtmlSlimParserTest, ScalableBlockFigureImageConsumesMarginsOnce) {
  renderer.scalableBaseSize = 12;
  expectChapterFigureLayout(CssTextAlign::Justify, 60);
}

TEST_F(ChapterHtmlSlimParserTest, ExplicitBlockFigureImageConsumesMarginsOnce) {
  expectChapterFigureLayout(CssTextAlign::Justify, 6, true);
}

TEST_F(ChapterHtmlSlimParserTest, BlockFigureImagesKeepBottomSpacingAfterTheLastImage) {
  cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 115;
  epub.optimizerImageHeight = 60;
  const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 6);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 66);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 162);
  EXPECT_EQ(parser.currentPageNextY, 178);
}

TEST_F(ChapterHtmlSlimParserTest, NestedFigureImageConsumesEachContainersClosingSpacingOnce) {
  expectNestedFigureClosingSpacing();
}

TEST_F(ChapterHtmlSlimParserTest, ScalableNestedFigureImageConsumesClosingSpacingOnce) {
  renderer.scalableBaseSize = 12;
  expectNestedFigureClosingSpacing();
}

TEST_F(ChapterHtmlSlimParserTest, BlockFigureImageKeepsCaptionBeforeClosingSpacing) {
  cssParser.rulesBySelector_["figure"] = CssParser::parseInlineStyle("margin-top: 6px; margin-bottom: 36px");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 115;
  epub.optimizerImageHeight = 60;
  const XML_Char* attributes[] = {"src", "art.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "figure", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::startElement(&parser, "figcaption", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Caption", 7);
  ChapterHtmlSlimParser::endElement(&parser, "figcaption");
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 6);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 66);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 118);
  EXPECT_EQ(parser.currentPageNextY, 134);
}

TEST_F(ChapterHtmlSlimParserTest, FigureWithoutEmbeddedStylesKeepsBlockBoundary) {
  parser.embeddedStyle = false;
  cssParser.rulesBySelector_["figure.inline_group"] =
      CssParser::parseInlineStyle("margin-top: 0.5em; margin-bottom: 3em; text-align: center");
  const XML_Char* attributes[] = {"class", "inline_group", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::startElement(&parser, "figure", attributes);
  const auto& style = parser.currentTextBlock->getBlockStyle();
  EXPECT_EQ(style.alignment, CssTextAlign::Justify);
  EXPECT_EQ(style.topInset(), 0);
  ChapterHtmlSlimParser::characterData(&parser, "Figure", 6);
  ChapterHtmlSlimParser::endElement(&parser, "figure");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  for (const auto& element : parser.currentPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    EXPECT_EQ(static_cast<const PageLine&>(*element).getBlock()->wordCount(), 1u);
  }
}

// Perfidia's scene break: a 27x25 image nested in a French span in a centered p.
TEST_F(ChapterHtmlSlimParserTest, HonorsPublisherCenterForImageOnlyParagraphWithReaderJustification) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Justify);
  parser.forceParagraphIndents = true;
  cssParser.rulesBySelector_[".MsoNormalCxSpFirst"] = CssParser::parseInlineStyle(
      "display: block; font-size: 1.33333em; margin: 0; text-align: center; text-indent: 0");
  cssParser.rulesBySelector_[".calibre9"] = CssParser::parseInlineStyle("width: 27; height: 25");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* paragraphAttributes[] = {"class", "MsoNormalCxSpFirst", nullptr};
  const XML_Char* spanAttributes[] = {"lang", "FR", nullptr};
  const XML_Char* imageAttributes[] = {"src", "image007.jpg", "class", "calibre9", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", paragraphAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "span", spanAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  // End-element cleanup must not lose the source alignment before layout.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Justify);
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, (parser.viewportWidth - 27) / 2);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageLine);
  EXPECT_EQ(static_cast<const PageLine&>(*parser.currentPage->elements[1]).getBlock()->getBlockStyle().alignment,
            CssTextAlign::Justify);
}

TEST_F(ChapterHtmlSlimParserTest, InheritsPublisherAlignmentForImageOnlyParagraph) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["div"] = CssParser::parseInlineStyle("text-align: right");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* imageAttributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, parser.viewportWidth - 27);
}

TEST_F(ChapterHtmlSlimParserTest, MixedImageParagraphKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("text-align: center");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* imageAttributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ImageOnlyAlignmentWorksAcrossReadingModesAndReaderPreferences) {
  for (const auto mode : {EpubRenderMode::CrossInkDefault, EpubRenderMode::Balanced, EpubRenderMode::Light}) {
    for (const auto readerAlignment :
         {CssTextAlign::Justify, CssTextAlign::Left, CssTextAlign::Right, CssTextAlign::None}) {
      for (const auto sourceAlignment : {"left", "center", "right"}) {
        SCOPED_TRACE(std::string(sourceAlignment) + ":" + std::to_string(int(mode)) + ":" +
                     std::to_string(int(readerAlignment)));
        parser.renderMode = mode;
        parser.paragraphAlignment = static_cast<uint8_t>(readerAlignment);
        cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle(std::string("text-align: ") + sourceAlignment);
        epub.optimizerImageAvailable = true;
        epub.optimizerImageWidth = 27;
        epub.optimizerImageHeight = 25;
        const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
        ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
        ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
        ChapterHtmlSlimParser::endElement(&parser, "img");
        ChapterHtmlSlimParser::endElement(&parser, "p");
        parser.makePages();
        ASSERT_NE(parser.currentPage, nullptr);
        ASSERT_EQ(parser.currentPage->elements.back()->getTag(), TAG_PageImage);
        const int expected = std::string(sourceAlignment) == "center"  ? (parser.viewportWidth - 27) / 2
                             : std::string(sourceAlignment) == "right" ? parser.viewportWidth - 27
                                                                       : 0;
        EXPECT_EQ(parser.currentPage->elements.back()->xPos, expected);
        parser.currentPage.reset();
        parser.currentPageNextY = 0;
        parser.currentTextBlock.reset();
      }
    }
  }
}

TEST_F(ChapterHtmlSlimParserTest, ImageOnlyTailAfterFlushedProseKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("text-align: center");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* imageAttributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  parser.flushLongTextRunIfNeeded(true, true);
  ASSERT_GT(parser.wordsExtractedInBlock, 0);
  ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.back()->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements.back()->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, InheritsBodyAlignmentAndRespectsExplicitParagraphOverride) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["body"] = CssParser::parseInlineStyle("text-align: center");
  cssParser.rulesBySelector_["p.left"] = CssParser::parseInlineStyle("text-align: left");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* imageAttributes[] = {"src", "icon.jpg", nullptr};
  const XML_Char* leftAttributes[] = {"class", "left", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "p", i == 0 ? nullptr : leftAttributes);
    ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, (parser.viewportWidth - 27) / 2);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, CentersMultipleImagesTogetherWithinPublisherInsets) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] =
      CssParser::parseInlineStyle("text-align: center; margin-left: 10px; margin-right: 20px");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  const int expected = 10 + (parser.viewportWidth - 30 - 54) / 2;
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, expected);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, expected + 27);
}

TEST_F(ChapterHtmlSlimParserTest, ImageAfterLineBreakInMixedParagraphKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("text-align: center");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::startElement(&parser, "br", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "br");
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ImageBeforeLineBreakAndCaptionKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("text-align: center");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::startElement(&parser, "br", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "br");
  ChapterHtmlSlimParser::characterData(&parser, "Caption", 7);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ImageAfterPreservedNewlineKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  const XML_Char* paragraph[] = {"style", "text-align: center; white-space: pre-wrap", nullptr};
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", paragraph);
  ChapterHtmlSlimParser::characterData(&parser, "Before\n", 7);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ImageBeforePreservedNewlineAndCaptionKeepsReaderAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  const XML_Char* paragraph[] = {"style", "text-align: center; white-space: pre-wrap", nullptr};
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", paragraph);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "\nCaption", 8);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  ASSERT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, ImageOnlyParagraphWithoutEmbeddedStylesKeepsReaderAlignment) {
  parser.embeddedStyle = false;
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("text-align: center");
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 27;
  epub.optimizerImageHeight = 25;
  const XML_Char* imageAttributes[] = {"src", "icon.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "img", imageAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements[0]->xPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, PlacesSmallImageInsideTextLine) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  const auto& line = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(line.getBlock()->wordCount(), 2u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements[1]);
  EXPECT_TRUE(image.isInlineImage());
  EXPECT_EQ(image.yPos, parser.currentPage->elements[0]->yPos);
  EXPECT_EQ(image.getImageBlock().getWidth(), 16);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
}

TEST_F(ChapterHtmlSlimParserTest, WrapsTextAfterInlineImageWithoutSplittingTheImage) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 22;
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "B", 1);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[2]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 4);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, parser.currentPage->elements[1]->yPos);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, AlignsTextWithTallerInlineImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 32;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 16);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 32);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitBlockDisplayForSmallImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: block", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_GE(parser.currentPage->elements[1]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitInlineDisplayForTallerImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 48;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: inline", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 32);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 48);
}

TEST_F(ChapterHtmlSlimParserTest, BoundsPendingImagesInAnIconOnlyParagraph) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1;
  epub.optimizerImageHeight = 1;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  for (int i = 0; i < 40; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    EXPECT_LT(parser.pendingInlineImages.size(), parser.MAX_PENDING_INLINE_IMAGES);
  }
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
  EXPECT_EQ(parser.currentPage->elements.size(), 40u);
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

namespace {
TEST_F(ChapterHtmlSlimParserTest, ScalableHeadingLevelsAndBodySizeCeiling) {
  renderer.scalableBaseSize = 12;
  const XML_Char* attrs[] = {nullptr};
  const uint8_t expected[] = {24, 18, 14, 12, 10, 8};
  for (int level = 1; level <= 6; ++level) {
    const char tag[] = {'h', static_cast<char>('0' + level), 0};
    ChapterHtmlSlimParser::startElement(&parser, tag, attrs);
    EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, expected[level - 1]);
    ChapterHtmlSlimParser::endElement(&parser, tag);
  }
  renderer.scalableBaseSize = 22;
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 44);
}

TEST_F(ChapterHtmlSlimParserTest, BlockSizesInheritAndRestoreAcrossSiblings) {
  renderer.scalableBaseSize = 12;
  const XML_Char* parent[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* child[] = {"style", "font-size: 0.5em", nullptr};
  const XML_Char* plain[] = {nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parent);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::startElement(&parser, "p", child);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 9);
  ChapterHtmlSlimParser::characterData(&parser, "Small", 5);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::characterData(&parser, "Parent", 6);
  ChapterHtmlSlimParser::startElement(&parser, "p", plain);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::startElement(&parser, "p", plain);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 12);
}

TEST_F(ChapterHtmlSlimParserTest, TextAfterTableKeepsParentFontSize) {
  renderer.scalableBaseSize = 12;
  const XML_Char* parent[] = {"style", "font-size: 150%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parent);
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "table");
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontScale, 384);
  ChapterHtmlSlimParser::characterData(&parser, "Parent", 6);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_FALSE(parser.currentPage->elements.empty());
  const auto& line = static_cast<const PageLine&>(*parser.currentPage->elements.back());
  EXPECT_EQ(line.getBlock()->getBlockStyle().fontSize, 18);
  EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 36);
}

TEST_F(ChapterHtmlSlimParserTest, RootRelativeSizesAndAbsoluteSizesRespectReaderZoom) {
  renderer.scalableBaseSize = 16;
  const XML_Char* html[] = {"style", "font-size: 125%", nullptr};
  const XML_Char* body[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* rem[] = {"style", "font-size: 1rem", nullptr};
  const XML_Char* points[] = {"style", "font-size: 12pt", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "html", html);
  ChapterHtmlSlimParser::startElement(&parser, "body", body);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 30);
  ChapterHtmlSlimParser::startElement(&parser, "p", rem);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 20);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::startElement(&parser, "p", points);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 16);
}

TEST_F(ChapterHtmlSlimParserTest, PublisherSizeOverridesHeadingDefaultAndInlineSizesStayUniform) {
  renderer.scalableBaseSize = 12;
  const XML_Char* heading[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* span[] = {"style", "font-size: 300%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::startElement(&parser, "span", span);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
}

TEST_F(ChapterHtmlSlimParserTest, BitmapAndLightModesKeepUniformSize) {
  const XML_Char* heading[] = {"style", "font-size: 200%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 0);
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  renderer.scalableBaseSize = 12;
  parser.renderMode = EpubRenderMode::Light;
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 0);
}

TEST_F(ChapterHtmlSlimParserTest, DisabledPublisherStylingKeepsSemanticHeadings) {
  renderer.scalableBaseSize = 12;
  parser.embeddedStyle = false;
  const XML_Char* heading[] = {"style", "font-size: 300%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h2", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
}

TEST_F(ChapterHtmlSlimParserTest, LargerHeadingsWrapAndReserveTheirActualHeight) {
  renderer.scalableBaseSize = 12;
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 100;
  const XML_Char* attrs[] = {nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  const char* text = "four four four four four four four four";
  ChapterHtmlSlimParser::characterData(&parser, text, std::strlen(text));
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_GT(parser.currentPage->elements.size(), 2u);
  int previousY = -48;
  for (const auto& element : parser.currentPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    const auto& line = static_cast<const PageLine&>(*element);
    EXPECT_EQ(line.getBlock()->getBlockStyle().fontSize, 24);
    EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 48);
    EXPECT_GE(line.yPos - previousY, 48);
    previousY = line.yPos;
  }
}

TEST(CssFontSizeTest, StrictValuesAndCascade) {
  for (const char* value : {"12badpx", "12foorem", "10vw", "12", "nanem", "-2em", "0px"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(std::string("font-size: ") + value).hasFontSize()) << value;
  }
  for (const char* value : {"150%", "1.25em", "1rem", "12pt", "16px", "larger", "small", "inherit"}) {
    EXPECT_TRUE(CssParser::parseInlineStyle(std::string("font-size: ") + value).hasFontSize()) << value;
  }
  const auto style = CssParser::parseInlineStyle("font-size: 150%; font-size: nonsense");
  EXPECT_TRUE(style.hasFontSize());
  EXPECT_FLOAT_EQ(style.fontSize.value, 150);
}
}  // namespace

TEST(CssBorderTest, SuppressionAndDeclarationOrder) {
  for (const char* declarations :
       {"border: none", "border: HIDDEN !important", "border: 0", "border: solid 0px black", "border-style: none",
        "border-width: 0rem", "border-width: 0 0px 0em 0pt", "border-style: none hidden",
        "border: solid; border-style: none", "border-width: 0; border-style: solid",
        "border: none; border-style: solid; border-width: 0",
        "border-top: none; border-right: 0; border-bottom: hidden; border-left-width: 0"}) {
    SCOPED_TRACE(declarations);
    const auto style = CssParser::parseInlineStyle(declarations);
    EXPECT_TRUE(style.defined.border);
    EXPECT_TRUE(style.suppressesHorizontalRule());
  }
  for (const char* declarations :
       {"", "border: solid", "border-style: dashed", "border-width: thin", "border-top: none",
        "border: none; border: 1px solid black", "border: none; border-top-style: solid", "border-width: 0 1px",
        "border: 0; border-width: 2px; border-style: solid", "border-style: none; border-style: solid"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(declarations).suppressesHorizontalRule()) << declarations;
  }
}

TEST(CssBorderTest, InvalidLonghandsDoNotOverrideSuppression) {
  for (const char* value : {"-1px", "1badpx", "nanpx", "2%", "2", "nonsense", "1px 1px 1px 1px 1px"}) {
    const auto style = CssParser::parseInlineStyle(std::string("border-width: 0; border-width: ") + value);
    EXPECT_TRUE(style.suppressesHorizontalRule()) << value;
  }
  EXPECT_TRUE(CssParser::parseInlineStyle("border-style: none; border-style: nonsense").suppressesHorizontalRule());
}

TEST(CssBorderTest, CascadeRestoresEdgesWithoutLosingZeroWidths) {
  auto style = CssParser::parseInlineStyle("border: none");
  style.applyOver(CssParser::parseInlineStyle("border-top-style: solid"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: 0"));
  EXPECT_TRUE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: medium"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.reset();
  EXPECT_FALSE(style.defined.border);
  EXPECT_FALSE(style.suppressesHorizontalRule());
}

TEST_F(ChapterHtmlSlimParserTest, CackleTransitionKeepsOnlyPublisherOrnament) {
  cssParser.rulesBySelector_["hr.transition"] = CssParser::parseInlineStyle("display: block; border: none; margin: 0");
  cssParser.rulesBySelector_["div.ornament"] = CssParser::parseInlineStyle("text-align: center; margin: 0");
  const XML_Char* hrAttrs[] = {"class", "transition", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", hrAttrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  EXPECT_EQ(parser.currentPageNextY, 0);
  const XML_Char* ornamentAttrs[] = {"class", "ornament", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", ornamentAttrs);
  ChapterHtmlSlimParser::characterData(&parser, "—", 3);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  // Starting the following paragraph seals the ornament's text block.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.depth, 1);
}

TEST_F(ChapterHtmlSlimParserTest, SuppressedRuleRetainsExplicitSpacing) {
  const XML_Char* attrs[] = {"style", "border: none; margin: 7px 0 9px; padding: 2px 0 3px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.currentPage->elements.empty());
  EXPECT_EQ(parser.currentPageNextY, 21);
}

TEST_F(ChapterHtmlSlimParserTest, PlainAndExplicitlyVisibleRulesStillRender) {
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  const XML_Char* attrs[] = {"style", "border: none; border-top: 1px solid black", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  for (const auto& element : parser.currentPage->elements) EXPECT_EQ(element->getTag(), TAG_PageHorizontalRule);
}

TEST_F(ChapterHtmlSlimParserTest, ParentBorderDoesNotHideChildRule) {
  const XML_Char* attrs[] = {"style", "border: none", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attrs);
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageHorizontalRule);
}

TEST_F(ChapterHtmlSlimParserTest, SoftFlushAppliesTopSpacingOnFirstEmittedLineOnly) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 40;
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  style.marginBottom = 9;
  style.paddingBottom = 3;
  style.textIndent = 8;
  style.textIndentDefined = true;
  parser.currentTextBlock->setBlockStyle(style);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true);
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentPageNextY, 0);
  EXPECT_FALSE(parser.currentTextBlock->isContinuation());
  for (int i = 0; i < 8; ++i) parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_GT(parser.currentPage->elements.size(), 0u);
  EXPECT_EQ(parser.currentPage->elements.front()->yPos, 12);
  const int emitted = parser.currentPage->elements.size();
  EXPECT_EQ(parser.currentPageNextY, 12 + emitted * 16);
  EXPECT_TRUE(parser.currentTextBlock->isContinuation());
  parser.makePages();
  EXPECT_EQ(parser.currentPageNextY, 24 + int(parser.currentPage->elements.size()) * 16);
  for (size_t i = 0; i < parser.currentPage->elements.size(); ++i) {
    EXPECT_EQ(parser.currentPage->elements[i]->yPos, 12 + int(i) * 16);
  }
}

TEST_F(ChapterHtmlSlimParserTest, EmptyFinalFlushDoesNotConsumeTopSpacing) {
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  parser.currentTextBlock->setBlockStyle(style);
  parser.makePages();
  EXPECT_EQ(parser.currentPageNextY, 0);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->yPos, 12);
}

TEST_F(ChapterHtmlSlimParserTest, FullyFlushedParagraphResetsSpacingForReusedBlock) {
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  parser.currentTextBlock->setBlockStyle(style);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true, true);
  EXPECT_EQ(parser.currentPageNextY, 28);
  parser.currentTextBlock->addWord("two", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true, true);
  EXPECT_EQ(parser.currentPageNextY, 44);
  parser.startNewTextBlock(style);
  parser.currentTextBlock->addWord("three", EpdFontFamily::REGULAR);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements.back()->yPos, 56);
}

TEST_F(ChapterHtmlSlimParserTest, LongParagraphKeepsWordAcrossCallbacksWhole) {
  expectWordAcrossLayoutThreshold("spli", "tword");
}

TEST_F(ChapterHtmlSlimParserTest, LongParagraphKeepsCyrillicWordAcrossCallbacksWhole) {
  expectWordAcrossLayoutThreshold("при", "мер");
}

TEST_F(ChapterHtmlSlimParserTest, LongParagraphKeepsCombiningMarkWithBaseAcrossCallbacks) {
  expectWordAcrossLayoutThreshold("क", "िताब");
}

TEST_F(ChapterHtmlSlimParserTest, ReadingAidByteLimitKeepsWordAcrossCallbacksWhole) {
  parser.focusReadingEnabled = true;
  expectWordAcrossLayoutThreshold("spli", "tword");
}

TEST_F(ChapterHtmlSlimParserTest, CombinedReadingAidByteLimitKeepsWordAcrossCallbacksWhole) {
  parser.focusReadingEnabled = true;
  parser.guideReadingEnabled = true;
  expectWordAcrossLayoutThreshold("spli", "tword");
}

TEST_F(ChapterHtmlSlimParserTest, FragmentAppendChecksBoundBeforeCallbackEnds) {
  renderer.textAdvancePerChar = 4;
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_GT(parser.wordsExtractedInBlock, 0);
  EXPECT_LE(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit());
}

TEST_F(ChapterHtmlSlimParserTest, HugeCjkCallbackDoesNotRetainWholeRunCapacity) {
  renderer.textAdvancePerChar = 4;
  std::string text;
  for (int i = 0; i < 6000; ++i) text += "\xE4\xB8\xAD";
  parser.completePageFn = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) {};
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  parser.flushPartWordBuffer();
  EXPECT_FALSE(parser.lowMemoryAbort);
  EXPECT_GT(parser.wordsExtractedInBlock, 5000);
  EXPECT_LT(parser.currentTextBlock->wordStyles.capacity(), 1024u);
  EXPECT_EQ(parser.visibleTextOffset, 6000u);
  EXPECT_EQ(parser.wordsExtractedInBlock + parser.currentTextBlock->size(), 6000u);
  if (!parser.currentTextBlock->isEmpty()) {
    EXPECT_EQ(parser.currentTextBlock->visibleOffsetAt(0), uint32_t(parser.wordsExtractedInBlock));
  }
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsRubyGroupBuffered) {
  renderer.textAdvancePerChar = 4;
  parser.inRuby = true;
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit() + 1);
  parser.inRuby = false;
  parser.flushLongTextRunIfNeeded();
  EXPECT_GT(parser.wordsExtractedInBlock, 0);
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsBufferedTableCellIntact) {
  renderer.textAdvancePerChar = 4;
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
  ASSERT_NE(parser.currentTableBuffer, nullptr);
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit() + 1);
}

TEST_F(ChapterHtmlSlimParserTest, SingleHugeCallbackHonorsPreviewPageLimit) {
  renderer.textAdvancePerChar = 4;
  parser.viewportHeight = 48;
  parser.previewAnchor = "note";
  parser.previewMaxPages = 1;
  parser.previewAnchorFound = true;
  int pages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) { ++pages; };
  std::string text;
  for (int i = 0; i < 2000; ++i) text += "word ";
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  EXPECT_TRUE(parser.previewStopRequested);
  EXPECT_EQ(pages, 1);
  EXPECT_EQ(parser.completedPageCount, 1);
}

TEST_F(ChapterHtmlSlimParserTest, SoftFlushLeavesTrailingFootnotesForFinalization) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 40;
  for (int i = 0; i < 8; ++i) parser.currentTextBlock->addWord("word", EpdFontFamily::REGULAR);
  FootnoteEntry note{};
  std::strcpy(note.number, "1");
  std::strcpy(note.href, "#note");
  note.linkId = 1;
  parser.pendingFootnotes.push_back({8, note});
  parser.flushLongTextRunIfNeeded(true);
  EXPECT_EQ(parser.pendingFootnotes.size(), 1u);
  parser.makePages();
  EXPECT_TRUE(parser.pendingFootnotes.empty());
  ASSERT_EQ(parser.currentPage->footnotes.size(), 1u);
  EXPECT_STREQ(parser.currentPage->footnotes[0].href, "#note");
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsActiveLinkOnEveryEmittedPage) {
  renderer.textAdvancePerChar = 4;
  parser.viewportHeight = 64;
  int linkedPages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
    ASSERT_EQ(page->footnotes.size(), 1u);
    EXPECT_STREQ(page->footnotes[0].href, "#note");
    ++linkedPages;
  };
  const XML_Char* attrs[] = {"href", "#note", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "a", attrs);
  std::string text;
  for (int i = 0; i < 1000; ++i) text += "linked ";
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  EXPECT_GT(linkedPages, 0);
  ChapterHtmlSlimParser::endElement(&parser, "a");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->footnotes.size(), 1u);
  EXPECT_STREQ(parser.currentPage->footnotes[0].href, "#note");
}

TEST_F(ChapterHtmlSlimParserTest, PageBreakBeforeDoesNotRepeatPreviousBottomSpacing) {
  BlockStyle previous;
  previous.marginBottom = 11;
  previous.paddingBottom = 7;
  parser.currentTextBlock->setBlockStyle(previous);
  parser.extraParagraphSpacing = true;
  parser.currentTextBlock->addWord("previous", EpdFontFamily::REGULAR);
  int pages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) { ++pages; };
  const XML_Char* attrs[] = {"style", "page-break-before: always", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "next", 4);
  parser.flushPartWordBuffer();
  parser.makePages();
  EXPECT_EQ(pages, 1);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, CallbackChunkingPreservesVisibleAndReferencePageOffsets) {
  renderer.textAdvancePerChar = 4;
  const std::string fragment = "word \xE4\xB8\xAD\xF0\x9F\x98\x80 text ";
  std::string text;
  for (int i = 0; i < 600; ++i) text += fragment;
  auto layout = [&](size_t chunkSize) {
    parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);
    parser.trackReferenceCharacters = true;
    parser.currentPage.reset();
    parser.currentPageNextY = 0;
    parser.completedPageCount = 0;
    parser.wordsExtractedInBlock = 0;
    parser.visibleTextOffset = 0;
    parser.referenceTextOffset = 0;
    parser.referenceTextStarted = false;
    parser.referenceWhitespacePending = false;
    parser.currentTextRunBytes = 0;
    parser.partWordBufferIndex = 0;
    parser.nextWordContinues = false;
    std::vector<std::array<uint32_t, 3>> pages;
    parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t visible, uint32_t reference) {
      pages.push_back({visible, reference, uint32_t(page->elements.size())});
    };
    for (size_t offset = 0; offset < text.size(); offset += chunkSize) {
      ChapterHtmlSlimParser::characterData(&parser, text.data() + offset, std::min(chunkSize, text.size() - offset));
    }
    parser.flushPartWordBuffer();
    parser.makePages();
    if (parser.currentPage && !parser.currentPage->elements.empty()) parser.completeCurrentPage();
    EXPECT_FALSE(parser.lowMemoryAbort);
    return pages;
  };
  const auto oneCallback = layout(text.size());
  const auto splitCallbacks = layout(fragment.size());
  EXPECT_GT(oneCallback.size(), 1u);
  EXPECT_EQ(oneCallback, splitCallbacks);
}

TEST_F(ChapterHtmlSlimParserTest, ContextSelectorsMatchIdsClassesAndDirectChildren) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("section.chapter > p.note.wide",
                                                  CssParser::parseInlineStyle("font-style: italic")));
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("#intro", CssParser::parseInlineStyle("font-weight: bold")));
  std::vector<CssAncestorEntry> ancestors{{0, "section", "chapter", ""}};
  auto style = cssParser.resolveStyle("p", "wide note", ancestors, "intro");
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  ancestors.push_back({1, "div", "", ""});
  EXPECT_FALSE(cssParser.resolveStyle("p", "wide note", ancestors).hasFontStyle());
}

TEST_F(ChapterHtmlSlimParserTest, FirstLetterRulesDoNotStyleWholeParagraph) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("p.opening::first-letter",
                                                  CssParser::parseInlineStyle("initial-letter: 3; font-weight: bold")));
  EXPECT_FALSE(cssParser.resolveStyle("p", "opening").hasFontWeight());
  const auto first = cssParser.resolveStyle("p", "opening", {}, {}, true);
  EXPECT_EQ(first.initialLetter, 3);
  const XML_Char* attrs[] = {"class", "opening", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hello world", 11);
  parser.flushPartWordBuffer();
  EXPECT_STREQ(parser.dropCap.text, "H");
  ASSERT_EQ(parser.currentTextBlock->words.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "ello");
  parser.makePages();
  ASSERT_TRUE(parser.currentPage);
  auto cap = std::find_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageDropCap; });
  ASSERT_NE(cap, parser.currentPage->elements.end());
  EXPECT_STREQ(static_cast<PageDropCap*>(cap->get())->getText(), "H");
  EXPECT_EQ(parser.visibleTextOffset, 11u);
}

TEST_F(ChapterHtmlSlimParserTest, OversizedDropCapSpanFallsBackWithoutLosingLetters) {
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const XML_Char* attrs[] = {"style", "float: left; font-size: 3em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hello", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  parser.flushPartWordBuffer();
  EXPECT_EQ(parser.dropCap.length, 0);
  ASSERT_EQ(parser.currentTextBlock->words.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Hello");
}

TEST_F(ChapterHtmlSlimParserTest, PreservedWhitespaceKeepsSpacesAndLineBreaks) {
  const XML_Char* attrs[] = {"style", "white-space: pre-wrap", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "A  B", 4);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->words.size(), 4u);
  EXPECT_EQ(parser.currentTextBlock->words[1], " ");
  EXPECT_EQ(parser.currentTextBlock->words[2], " ");
  EXPECT_TRUE(parser.currentTextBlock->wordNoSpaceBefore.back());
  ChapterHtmlSlimParser::characterData(&parser, "\nC", 2);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->words.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "C");
  EXPECT_EQ(parser.visibleTextOffset, 6u);
}

TEST_F(ChapterHtmlSlimParserTest, WhitespaceOverrideRestoresParentStyle) {
  const XML_Char* pre[] = {"style", "white-space: pre-wrap", nullptr};
  const XML_Char* normal[] = {"style", "white-space: normal", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", pre);
  EXPECT_TRUE(parser.effectivePreserveWhitespace);
  ChapterHtmlSlimParser::startElement(&parser, "span", normal);
  EXPECT_FALSE(parser.effectivePreserveWhitespace);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_TRUE(parser.effectivePreserveWhitespace);
}

TEST_F(ChapterHtmlSlimParserTest, BorderLonghandsPreserveOtherSidesAndWidth) {
  auto style = CssParser::parseInlineStyle("border: 2px dashed; border-left-style: none; border-top-width: 4px");
  EXPECT_TRUE(style.hasVisibleBorder());
  EXPECT_EQ(style.borders[0].width, 4);
  EXPECT_EQ(style.borders[1].style, CssBorderStyle::Dashed);
  EXPECT_FALSE(style.borders[3].visible());
  style.applyOver(CssParser::parseInlineStyle("border-left-style: solid"));
  EXPECT_EQ(style.borders[3].width, 2);
  EXPECT_EQ(style.borders[3].style, CssBorderStyle::Solid);
}

TEST_F(ChapterHtmlSlimParserTest, BorderedParagraphAddsABoxAfterItsText) {
  const XML_Char* attrs[] = {"style", "border: 2px solid; background-color: silver", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  EXPECT_EQ(parser.boxScopeCount, 1u);
  ChapterHtmlSlimParser::characterData(&parser, "Framed text", 11);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.boxScopeCount, 0u);
  ASSERT_TRUE(parser.currentPage);
  EXPECT_EQ(std::count_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageBorderBox; }),
            1);
}

TEST_F(ChapterHtmlSlimParserTest, BareFieldsetFramesChildParagraphsAndRestoresFollowingText) {
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  ASSERT_EQ(parser.boxScopeCount, 1u);
  const auto fieldsetStyle = parser.currentTextBlock->getBlockStyle();
  EXPECT_GT(fieldsetStyle.leftInset(), 2);
  for (const auto& side : parser.boxScopes[0].sides) {
    EXPECT_TRUE(side.visible());
    EXPECT_EQ(side.width, 1);
  }
  for (const char* text : {"This is a test", "Second paragraph"}) {
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), fieldsetStyle.leftInset());
    ChapterHtmlSlimParser::characterData(&parser, text, std::strlen(text));
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  EXPECT_EQ(parser.boxScopeCount, 0u);
  EXPECT_EQ(parser.blockStyleCount_, 1u);
  ASSERT_TRUE(parser.currentPage);
  EXPECT_EQ(std::count_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageBorderBox; }),
            1);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 0);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().paddingTop, 0);
}

TEST_F(ChapterHtmlSlimParserTest, FieldsetCssOverridesDefaultsWithoutResettingOtherEdges) {
  cssParser.rulesBySelector_["fieldset"] = CssParser::parseInlineStyle("border-width: 2px; padding-left: 20px");
  const XML_Char* attrs[] = {"style", "border-top-style: dashed; border-left: none; margin-right: 10px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", attrs);
  ASSERT_EQ(parser.boxScopeCount, 1u);
  const auto& box = parser.boxScopes[0];
  EXPECT_EQ(box.sides[0].style, CssBorderStyle::Dashed);
  EXPECT_EQ(box.sides[0].width, 2);
  EXPECT_EQ(box.sides[1].style, CssBorderStyle::Solid);
  EXPECT_EQ(box.sides[1].width, 2);
  EXPECT_FALSE(box.sides[3].visible());
  EXPECT_EQ(box.left, 2);
  EXPECT_EQ(box.right, parser.viewportWidth - 10);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().paddingLeft, 20);
}

TEST_F(ChapterHtmlSlimParserTest, FieldsetBorderCanBeSuppressedByPublisherCss) {
  cssParser.rulesBySelector_["fieldset"] = CssParser::parseInlineStyle("border: none; padding: 0; margin: 0");
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  EXPECT_EQ(parser.boxScopeCount, 0u);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 0);
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  const XML_Char* attrs[] = {"style", "border: 0", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", attrs);
  EXPECT_EQ(parser.boxScopeCount, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, FieldsetDefaultsWorkWithoutCssParser) {
  parser.cssParser = nullptr;
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  EXPECT_EQ(parser.boxScopeCount, 1u);
}

TEST_F(ChapterHtmlSlimParserTest, FieldsetRespectsDisabledEmbeddedStylesAndLightMode) {
  parser.embeddedStyle = false;
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  EXPECT_EQ(parser.boxScopeCount, 0u);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 0);
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  parser.embeddedStyle = true;
  parser.renderMode = EpubRenderMode::Light;
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  EXPECT_EQ(parser.boxScopeCount, 0u);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 0);
}

TEST_F(ChapterHtmlSlimParserTest, NestedFieldsetsKeepIndependentBoxes) {
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  ASSERT_EQ(parser.boxScopeCount, 2u);
  EXPECT_GT(parser.boxScopes[1].left, parser.boxScopes[0].left);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Nested", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  EXPECT_EQ(parser.boxScopeCount, 1u);
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  EXPECT_EQ(parser.boxScopeCount, 0u);
  ASSERT_TRUE(parser.currentPage);
  EXPECT_EQ(std::count_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageBorderBox; }),
            2);
}

TEST_F(ChapterHtmlSlimParserTest, LongFieldsetEmitsOneBoxPerPageAndClosesBeforeFollowingText) {
  renderer.textAdvancePerChar = 4;
  parser.viewportHeight = 64;
  int framedPages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
    EXPECT_EQ(std::count_if(page->elements.begin(), page->elements.end(),
                            [](const auto& item) { return item->getTag() == TAG_PageBorderBox; }),
              1);
    ++framedPages;
  };
  ChapterHtmlSlimParser::startElement(&parser, "fieldset", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  std::string text;
  for (int i = 0; i < 500; ++i) text += "word ";
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "fieldset");
  ASSERT_GT(framedPages, 1);
  EXPECT_FALSE(parser.lowMemoryAbort);
  EXPECT_EQ(parser.boxScopeCount, 0u);
  ASSERT_TRUE(parser.currentPage);
  parser.completeCurrentPage();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 0);
}

TEST_F(ChapterHtmlSlimParserTest, InlineFontSizesMeasureAndRestoreWithinOneLine) {
  renderer.scalableBaseSize = 12;
  renderer.textAdvancePerChar = 6;
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Body ", 5);
  const XML_Char* attrs[] = {"style", "font-size: 2em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Large", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " tail", 5);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->wordFontSizes.size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[0], 0);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[1], 24);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[2], 0);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& line = static_cast<PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(line.getBlock()->wordFontSize(1), 24);
  EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 48);
}

TEST_F(ChapterHtmlSlimParserTest, NestedInlineSizesUseParentAndRestore) {
  renderer.scalableBaseSize = 12;
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const XML_Char* big[] = {"style", "font-size: 2em", nullptr};
  const XML_Char* small[] = {"style", "font-size: 50%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", big);
  EXPECT_EQ(parser.effectiveInlineFontSize, 24);
  ChapterHtmlSlimParser::startElement(&parser, "span", small);
  EXPECT_EQ(parser.effectiveInlineFontSize, 12);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_EQ(parser.effectiveInlineFontSize, 24);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_EQ(parser.effectiveInlineFontSize, 0);
}

TEST_F(ChapterHtmlSlimParserTest, CssHiddenIdsDoNotBecomeAnchorsOrPageMarkers) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("#hidden-chapter", CssParser::parseInlineStyle("display: none")));
  parser.tocAnchors.push_back("hidden-chapter");
  const XML_Char* attrs[] = {"id", "hidden-chapter", "role", "doc-pagebreak", "title", "42", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hidden", 6);
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  EXPECT_TRUE(parser.pendingAnchorId.empty());
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  EXPECT_TRUE(parser.anchorData.empty());
  EXPECT_EQ(parser.completedPageCount, 0);
  ChapterHtmlSlimParser::endElement(&parser, "p");
}

TEST_F(ChapterHtmlSlimParserTest, BorderedParagraphKeepsClosingPaddingAndMargin) {
  const XML_Char* attrs[] = {"style", "border:2px solid;padding-bottom:20px;margin-bottom:20px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_TRUE(parser.currentPage);
  int textBottom = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() == TAG_PageLine) textBottom = element->yPos + renderer.getLineHeight(0);
  }
  EXPECT_GE(parser.currentPageNextY, textBottom + 42);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "B", 1);
  parser.flushPartWordBuffer();
  parser.makePages();
  const auto& last = parser.currentPage->elements.back();
  ASSERT_EQ(last->getTag(), TAG_PageLine);
  EXPECT_GE(last->yPos, textBottom + 42);
}

TEST_F(ChapterHtmlSlimParserTest, ChildSelectorKeepsAlternativeAncestorMatches) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("section > div p", CssParser::parseInlineStyle("font-weight:bold")));
  const std::vector<CssAncestorEntry> ancestors{{0, "section", "", ""}, {1, "div", "", ""}, {2, "div", "", ""}};
  EXPECT_EQ(cssParser.resolveStyle("p", "", ancestors).fontWeight, CssFontWeight::Bold);
}

TEST_F(ChapterHtmlSlimParserTest, StandaloneInitialLetterPreservesWordBoundary) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("p::first-letter", CssParser::parseInlineStyle("initial-letter:3")));
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "I", 1);
  ChapterHtmlSlimParser::characterData(&parser, " am here", 8);
  parser.flushPartWordBuffer();
  EXPECT_EQ(parser.dropCap.length, 0);
  ASSERT_EQ(parser.currentTextBlock->words.size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "I");
  EXPECT_EQ(parser.currentTextBlock->words[1], "am");
  EXPECT_EQ(parser.currentTextBlock->words[2], "here");
}

TEST(PublisherFontCss, FamiliesAreCaseInsensitiveAndGenericNamesKeepReaderFont) {
  const auto body = CssParser::parseInlineStyle("font-family: 'Book Body', serif");
  EXPECT_TRUE(body.defined.fontFamily);
  EXPECT_EQ(body.fontFamily, CssParser::hashFontFamily("book body"));
  EXPECT_EQ(CssParser::parseInlineStyle("font-family: serif").fontFamily, 0u);
  EXPECT_FALSE(CssParser::parseInlineStyle("font-family: inherit").defined.fontFamily);
  auto inherited = body;
  inherited.applyOver(CssParser::parseInlineStyle("font-family: sans-serif"));
  EXPECT_EQ(inherited.fontFamily, 0u);
}

TEST(PublisherFontCss, FindsSupportedSourceAndBoundsManifest) {
  CssParser css{"/tmp"};
  css.parseFontFace(
      "font-family: 'Book Body'; src: url('body.woff2'), url('../Fonts/body.otf'); font-weight: 700; font-style: "
      "italic",
      "OEBPS/Styles/");
  ASSERT_EQ(css.fontFaces_.size(), 1u);
  EXPECT_EQ(css.fontFaces_[0].href, "OEBPS/Styles/../Fonts/body.otf");
  EXPECT_TRUE(css.fontFaces_[0].bold);
  EXPECT_TRUE(css.fontFaces_[0].italic);
  css.parseFontFace("font-family: NoSource; src: url('body.woff2')", "");
  EXPECT_EQ(css.fontFaces_.size(), 1u);
  for (unsigned i = 0; i < 100; ++i) css.parseFontFace("font-family: Body; src: url(body.ttf)", "");
  EXPECT_EQ(css.fontFaces_.size(), 32u);
  css.clear();
  EXPECT_TRUE(css.fontFaces_.empty());
}

TEST_F(ChapterHtmlSlimParserTest, PublisherModesPreserveReaderBodyAndAllowAccents) {
  const uint32_t body = CssParser::hashFontFamily("Body");
  const uint32_t accent = CssParser::hashFontFamily("Accent");
  parser.bodyFontFamily = body;
  parser.inlineStyleCount_ = 1;
  inlineStyles[0].hasFontFamily = true;
  inlineStyles[0].fontFamily = body;
  parser.setPublisherFonts(PublisherFonts::Off);
  parser.updateEffectiveInlineStyle();
  EXPECT_EQ(parser.effectiveFontFamily, 0u);
  parser.setPublisherFonts(PublisherFonts::All);
  parser.updateEffectiveInlineStyle();
  EXPECT_EQ(parser.effectiveFontFamily, body);
  parser.setPublisherFonts(PublisherFonts::Accents);
  parser.updateEffectiveInlineStyle();
  EXPECT_EQ(parser.effectiveFontFamily, 0u);
  inlineStyles[0].fontFamily = accent;
  parser.updateEffectiveInlineStyle();
  EXPECT_EQ(parser.effectiveFontFamily, accent);
  parser.embeddedStyle = false;
  parser.updateEffectiveInlineStyle();
  EXPECT_EQ(parser.effectiveFontFamily, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, InlinePublisherFamilyEndsWithItsSpan) {
  parser.setPublisherFonts(PublisherFonts::All);
  const XML_Char* attrs[] = {"style", "font-family: Accent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Reader ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Accent ", 7);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "Reader ", 7);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->wordFontFamilies.size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->wordFontFamilies[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordFontFamilies[1], CssParser::hashFontFamily("Accent"));
  EXPECT_EQ(parser.currentTextBlock->wordFontFamilies[2], 0u);
}

TEST(PublisherFontCache, ModeAndAvailableFacesChangeLayoutIdentity) {
  ReaderRenderSpec spec;
  const auto off = readerRenderSpecSignature(spec);
  spec.publisherFonts = PublisherFonts::Accents;
  const auto accents = readerRenderSpecSignature(spec);
  EXPECT_NE(off, accents);
  spec.publisherFonts = PublisherFonts::All;
  EXPECT_NE(accents, readerRenderSpecSignature(spec));
  const auto full = readerRenderSpecSignature(spec);
  spec.publisherFontSignature = 123;
  EXPECT_NE(full, readerRenderSpecSignature(spec));
}

TEST_F(ChapterHtmlSlimParserTest, AccentsProbeFindsParagraphBodyFontBeforeHeadings) {
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("font-family: Body");
  cssParser.rulesBySelector_["h1"] = CssParser::parseInlineStyle("font-family: Accent");
  parser.parseFile_.setContents(
      "<html><body><h1>A long decorative chapter heading</h1><p>Normal body text.</p></body></html>");
  parser.activeParser = XML_ParserCreate(nullptr);
  ASSERT_NE(parser.activeParser, nullptr);
  ASSERT_TRUE(parser.detectBodyFont());
  EXPECT_EQ(parser.bodyFontFamily, CssParser::hashFontFamily("Body"));
  EXPECT_EQ(parser.parseFile_.position(), 0u);
}

TEST_F(ChapterHtmlSlimParserTest, AccentsProbeDoesNotTreatInlineAccentsAsBody) {
  cssParser.rulesBySelector_["p"] = CssParser::parseInlineStyle("font-family: Body");
  parser.parseFile_.setContents(
      "<html><body><p><span style='font-family: Accent'>A very long accent.</span> Body.</p></body></html>");
  parser.activeParser = XML_ParserCreate(nullptr);
  ASSERT_TRUE(parser.detectBodyFont());
  EXPECT_EQ(parser.bodyFontFamily, CssParser::hashFontFamily("Body"));
}

TEST_F(ChapterHtmlSlimParserTest, BodyFontProbeSkipsInvisibleTitlesAndRubyFallback) {
  const std::string hidden(512, 'x');
  parser.parseFile_.setContents(
      "<html><body><p style='font-family: Decoration'>A<svg><title>" + hidden + "</title></svg><ruby>B<rp>" + hidden +
      "</rp></ruby></p><p style='font-family: Body'>Ordinary prose is the body font.</p></body></html>");
  parser.activeParser = XML_ParserCreate(nullptr);
  ASSERT_TRUE(parser.detectBodyFont());
  EXPECT_EQ(parser.bodyFontFamily, CssParser::hashFontFamily("Body"));
  EXPECT_EQ(parser.parseFile_.position(), 0u);
}

TEST(PublisherFontCss, StreamsFontFaceAndNormalRulesTogether) {
  CssParser css{"/tmp"};
  HalFile file;
  file.setContents("@font-face { font-family: Body; src: url('../Fonts/body.ttf'); } p { font-family: Body; }");
  ASSERT_TRUE(css.loadFromStream(file, "OEBPS/Styles/"));
  ASSERT_EQ(css.fontFaces_.size(), 1u);
  EXPECT_EQ(css.fontFaces_[0].family, CssParser::hashFontFamily("body"));
}

TEST(PublisherFonts, ImportantFamilyUsesTheSameIdentity) {
  const auto style = CssParser::parseInlineStyle("font-family: 'Publisher Body' !important");
  EXPECT_TRUE(style.defined.fontFamily);
  EXPECT_EQ(style.fontFamily, CssParser::hashFontFamily("Publisher Body"));
  EXPECT_FALSE(CssParser::parseInlineStyle("font-family: inherit !important").defined.fontFamily);
}

TEST_F(ChapterHtmlSlimParserTest, DisabledFamilySpanPreservesFocusWordBoundary) {
  parser.setPublisherFonts(PublisherFonts::Off);
  parser.focusReadingEnabled = true;
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "ord", 3);
  const XML_Char* attrs[] = {"style", "font-family: Accent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "inary", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->words.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "ordinary");
  EXPECT_TRUE(parser.currentTextBlock->wordFontFamilies.empty());
}

TEST_F(ChapterHtmlSlimParserTest, ChunkedAnchorsRetainOrderPagesAndReferencesAcrossGrowth) {
  for (int i = 0; i < 1100; ++i) {
    parser.pendingAnchorId = "anchor-" + std::to_string(i);
    parser.completedPageCount = i;
    parser.flushPendingAnchor();
  }
  const auto& anchors = parser.getAnchors();
  ASSERT_EQ(anchors.size(), 1100u);
  const auto* first = &anchors.front();
  for (int i = 1100; i < 2200; ++i) {
    parser.pendingAnchorId = "anchor-" + std::to_string(i);
    parser.completedPageCount = i;
    parser.flushPendingAnchor();
  }
  EXPECT_EQ(first, &anchors.front());
  for (size_t i = 0; i < anchors.size(); ++i) {
    EXPECT_EQ(anchors[i].first, "anchor-" + std::to_string(i));
    EXPECT_EQ(anchors[i].second, i);
  }
}

TEST(ImageDimsProbe, GifSignaturesAndLittleEndianDimensions) {
  for (const char version : {'7', '9'}) {
    ImageDimsProbe probe;
    const uint8_t bytes[] = {'G', 'I', 'F', '8', static_cast<uint8_t>(version), 'a', 0x00, 0x02, 0x20, 0x03};
    probe.write(bytes, sizeof(bytes));
    ImageDimensions dims{};
    ASSERT_TRUE(probe.getDimensions(dims));
    EXPECT_EQ(dims.width, 512);
    EXPECT_EQ(dims.height, 800);
  }
  ImageDimsProbe invalid;
  const uint8_t bytes[] = {'G', 'I', 'F', '8', '8', 'a', 1, 0, 1, 0};
  invalid.write(bytes, sizeof(bytes));
  ImageDimensions dims{};
  EXPECT_FALSE(invalid.getDimensions(dims));
}
