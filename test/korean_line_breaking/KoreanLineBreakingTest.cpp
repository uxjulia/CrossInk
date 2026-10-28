#include <Epub/ParsedText.h>
#include <Epub/WordSpacing.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
};

// Fixture metrics (stub renderer): every glyph is 8 px wide and a space is 4 px.
std::vector<Line> layout(const std::vector<const char*>& words, const bool hyphenation, const uint16_t width) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, hyphenation, false, false, 0, style);
  for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  EXPECT_TRUE(text.layoutAndExtractLines(renderer, 0, width, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    auto& line = lines.emplace_back();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      line.words.emplace_back(block->wordText(i));
      line.xpos.push_back(block->wordXpos(i));
    }
  }));
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  result.reserve(lines.size());
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

}  // namespace

TEST(KoreanLineBreaking, HyphenationOffWrapsAtSpacesOnly) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나 다라마바 needs 52 px; with hyphenation off the Hangul word moves down whole.
  const auto lines = layout({"가나", "다라마바", "사"}, false, 50);
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"다라마바", "사"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenSyllablesWithoutHyphen) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나 + space leaves 30 px: the widest syllable prefix that fits is 다라마 (24 px).
  const auto lines = layout({"가나", "다라마바사아", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나", "다라마"}, {"바사아", "자"}};
  ASSERT_EQ(wordsOf(lines), expected);
  // 16 + 4 + 24 = 44 px leaves 6 px, all of it on the one word space.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 26}));
}

TEST(KoreanLineBreaking, HyphenationOnKeepsTrailingPunctuationWithSyllable) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 36 px. 라마바사 (32 px) would fit, but 사. stays together, so 라마바 is used.
  const auto lines = layout({"가나다", "라마바사.", "자"}, true, 64);
  const std::vector<std::vector<std::string>> expected{{"가나다", "라마바"}, {"사.", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsShortWords) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 22 px, so the three-syllable word splits after 라마 (16 px).
  const auto lines = layout({"가나다", "라마바", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"바", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsWhereHangulMeetsOtherScriptsOrBrackets) {
  Hyphenator::setPreferredLanguage("ko");
  // 가 + space leaves 52 px. 소신(ab) (48 px) fits; the split may fall before "(" or after ")",
  // never inside the brackets.
  const auto lines = layout({"가", "소신(ab)이"}, true, 64);
  const std::vector<std::vector<std::string>> expected{{"가", "소신(ab)"}, {"이"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenDigitAndHangul) {
  Hyphenator::setPreferredLanguage("ko");
  // 가나다 + space leaves 22 px: 12 (16 px) fits, 12월 (24 px) does not.
  const auto lines = layout({"가나다", "12월부터", "자"}, true, 50);
  const std::vector<std::vector<std::string>> expected{{"가나다", "12"}, {"월부터", "자"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, JustifiedWordsKeepNaturalSyllableSpacing) {
  Hyphenator::setPreferredLanguage("ko");
  const auto lines = layout({"가나", "다라", "마바", "사아"}, false, 60);
  const std::vector<std::vector<std::string>> expected{{"가나", "다라", "마바"}, {"사아"}};
  EXPECT_EQ(wordsOf(lines), expected);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 22, 44}));
}

TEST(KoreanLineBreaking, HangulGluedAcrossInlineStyleStaysTogether) {
  Hyphenator::setPreferredLanguage("ko");
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, false, false, 0, style);
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  std::vector<std::vector<std::string>> lines;
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 40, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    auto& words = lines.emplace_back();
    for (uint16_t i = 0; i < block->wordCount(); ++i) words.emplace_back(block->wordText(i));
  }));
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"한국", "어"}};
  EXPECT_EQ(lines, expected);
}

TEST(KoreanLineBreaking, NegativeWordSpacingTightensLayoutWithoutOverlapping) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  int previous = 1000;
  for (int level = 0; level >= -4; --level) {
    ParsedText text(false, false, false, false, false, WordSpacing::fromLevel(level), style);
    text.addWord("AA", EpdFontFamily::REGULAR);
    text.addWord("BB", EpdFontFamily::REGULAR);
    int position = 0;
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 200, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
      ASSERT_EQ(block->wordCount(), 2);
      position = block->wordXpos(1);
    }));
    EXPECT_GT(position, 16);
    EXPECT_LE(position, previous);
    previous = position;
  }
  EXPECT_EQ(previous, 17);
}

TEST(EmDashLineBreaking, HyphenationOffSplitsAcrossLines) {
  // "foo—" is 4 chars * 8 px = 32 px, fits in 40 px.
  // "bar" is 3 chars * 8 px = 24 px. Total "foo—bar" is 56 px, which exceeds 40 px.
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/false, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, HyphenationOffKeepsTogetherOnSameLineWithoutGap) {
  // 80 px fits "foo—" (32 px) and "bar" (24 px).
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/false, 80);
  const std::vector<std::vector<std::string>> expected{{"foo—", "bar"}};
  ASSERT_EQ(wordsOf(lines), expected);
  // "bar" attaches directly to "foo—" with kerning 0, so xpos is exactly 0 and 32.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 32}));
}

TEST(EmDashLineBreaking, MultiEmDashStaysTogether) {
  // "foo——" is 5 chars * 8 px = 40 px. Width 45 fits "foo——", but not "bar".
  const auto lines = layout({"foo——bar"}, /*hyphenation=*/false, 45);
  const std::vector<std::vector<std::string>> expected{{"foo——"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MultipleEmDashesInWordCanBreakAtEitherDash) {
  // "foo—" (32 px), "bar—" (32 px), "baz" (24 px). Width 40 px.
  const auto lines = layout({"foo—bar—baz"}, /*hyphenation=*/false, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar—"}, {"baz"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, HyphenationOnSplitsWithoutInsertedHyphen) {
  const auto lines = layout({"foo—bar"}, /*hyphenation=*/true, 40);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedCjkAndLatinSplitsAtDash) {
  // "漢" (8 px), "字—" (16 px), "bar" (24 px). Width 30 px fits "漢字—" (24 px), but not "bar".
  const auto lines = layout({"漢字—bar"}, /*hyphenation=*/false, 30);
  const std::vector<std::vector<std::string>> expected{{"漢", "字—"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedLatinAndCjkSplitsAtDash) {
  // "foo—" (32 px), "漢" (8 px), "字" (8 px). Width 35 px fits "foo—", but wraps CJK text.
  const auto lines = layout({"foo—漢字"}, /*hyphenation=*/false, 35);
  const std::vector<std::vector<std::string>> expected{{"foo—"}, {"漢", "字"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, MixedCjkMultiEmDashStaysTogether) {
  // "漢" (8 px), "字——" (24 px), "bar" (24 px). Width 35 px fits "漢字——", but wraps "bar".
  const auto lines = layout({"漢字——bar"}, /*hyphenation=*/false, 35);
  const std::vector<std::vector<std::string>> expected{{"漢", "字——"}, {"bar"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(EmDashLineBreaking, LeadingDashRunStaysWithFollowingText) {
  EXPECT_EQ(wordsOf(layout({"a", "——bar"}, false, 44)), (std::vector<std::vector<std::string>>{{"a"}, {"——bar"}}));
}

TEST(EmDashLineBreaking, NonBreakingHyphenRemainsUnbroken) {
  EXPECT_EQ(wordsOf(layout({"a", "foo‑bar"}, false, 60)), (std::vector<std::vector<std::string>>{{"a"}, {"foo‑bar"}}));
}

TEST(EmDashLineBreaking, FocusReadingAndOffsetsSurviveTokenSplit) {
  GfxRenderer renderer;
  BlockStyle style;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, true, false, 0, style, true);
  text.addWord("alpha—bravo", EpdFontFamily::REGULAR, false, false, false, 3, 100, 200);
  int line = 0;
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 48,
                                         [&](std::shared_ptr<TextBlock> block, uint32_t visible, uint32_t reference) {
                                           ASSERT_EQ(block->wordCount(), 1);
                                           EXPECT_STREQ(block->wordText(0), line == 0 ? "alpha—" : "bravo");
                                           EXPECT_EQ(visible, line == 0 ? 100u : 106u);
                                           EXPECT_EQ(reference, line == 0 ? 200u : 206u);
                                           EXPECT_GT(block->focusBoundary(0), 0);
                                           EXPECT_EQ(block->wordLinkId(0), 3);
                                           ++line;
                                         }));
  EXPECT_EQ(line, 2);
}

TEST(EmDashLineBreaking, InlineStyleBoundaryAfterDashIsBreakable) {
  GfxRenderer renderer;
  BlockStyle style;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, false, false, 0, style);
  text.addWord("foo—", EpdFontFamily::REGULAR);
  text.addWord("bar", EpdFontFamily::BOLD, false, true);
  std::vector<std::string> lines;
  ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 32, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    ASSERT_EQ(block->wordCount(), 1);
    lines.emplace_back(block->wordText(0));
  }));
  EXPECT_EQ(lines, (std::vector<std::string>{"foo—", "bar"}));
}

TEST(EmDashLineBreaking, JustificationDoesNotStretchInsideCompound) {
  for (const bool hyphenation : {false, true}) {
    for (const char* compound : {"place-card", "place–card", "place—card"}) {
      SCOPED_TRACE(compound);
      const auto lines = layout({"a", compound, "holders", "xxxxxxxxxxxxxxxx"}, hyphenation, 180);
      ASSERT_GE(lines.size(), 2u);
      ASSERT_EQ(lines[0].words.size(), 4u);
      EXPECT_EQ(lines[0].words[2], "card");
      // Only the two real word spaces share the spare width (28 px).
      EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 26, 74, 124}));
    }
  }
}

TEST(EmDashLineBreaking, RealSpaceAfterHyphenStillExpands) {
  const auto lines = layout({"a", "place-", "card", "holders", "xxxxxxxxxxxxxxxx"}, false, 180);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 20, 80, 124}));
}

TEST(EmDashLineBreaking, CjkBoundariesStillJustify) {
  const auto lines = layout({"漢字漢字漢字"}, false, 38);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 20, 30}));
}

TEST(KoreanLineBreaking, MixedScriptsKeepBoundariesTouchingHangulTogether) {
  const auto lines = layout({"ab中가나國cd文"}, false, 300);
  const std::vector<std::vector<std::string>> expected{{"ab", "中가나國", "cd", "文"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, ExplicitHyphensBeforeFirstCjkCharacterRemainBreaks) {
  const auto lines = layout({"ab-cd가-나中"}, false, 300);
  const std::vector<std::vector<std::string>> expected{{"ab-", "cd가-", "나中"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, LatinHyphenRunsKeepTheNonCjkPath) {
  const auto lines = layout({"ab--cd"}, false, 300);
  const std::vector<std::vector<std::string>> expected{{"ab--", "cd"}};
  EXPECT_EQ(wordsOf(lines), expected);
}

TEST(KoreanLineBreaking, CombiningMarksAndCjkPunctuationStayAttached) {
  const auto lines = layout({"（中）文\xCC\x81字"}, false, 300);
  const std::vector<std::vector<std::string>> expected{{"（中）", "文\xCC\x81", "字"}};
  EXPECT_EQ(wordsOf(lines), expected);
}
