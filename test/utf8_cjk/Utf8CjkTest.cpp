#include <Utf8.h>
#include <gtest/gtest.h>

TEST(Utf8CjkTest, DetectsCjkBreakableRanges) {
  EXPECT_TRUE(utf8IsCjkBreakable(0x4E16));   // CJK unified ideograph.
  EXPECT_TRUE(utf8IsCjkBreakable(0x3042));   // Hiragana.
  EXPECT_TRUE(utf8IsCjkBreakable(0xAC00));   // Hangul syllable.
  EXPECT_TRUE(utf8IsCjkBreakable(0x20000));  // CJK Extension B.

  EXPECT_FALSE(utf8IsCjkBreakable('A'));
  EXPECT_FALSE(utf8IsCjkBreakable(0x03BB));  // Greek lambda.
}

TEST(Utf8CjkTest, ClassifiesPunctuationForLineBreakRules) {
  EXPECT_TRUE(utf8IsCjkOpeningPunctuation(0x300C));  // left corner bracket.
  EXPECT_TRUE(utf8IsCjkOpeningPunctuation(0xFF08));  // fullwidth left parenthesis.
  EXPECT_FALSE(utf8IsCjkOpeningPunctuation(0x300D));

  EXPECT_TRUE(utf8IsCjkClosingPunctuation(0x300D));  // right corner bracket.
  EXPECT_TRUE(utf8IsCjkClosingPunctuation(0x3001));  // ideographic comma.
  EXPECT_TRUE(utf8IsCjkClosingPunctuation(0xFF1F));  // fullwidth question mark.
  EXPECT_FALSE(utf8IsCjkClosingPunctuation(0x300C));
}

TEST(Utf8CjkTest, DetectsCjkLanguageTags) {
  EXPECT_TRUE(utf8LanguageTagIsCjk("ja"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("jpn"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("zh-Hans"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("zho"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("chi"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("KO"));
  EXPECT_TRUE(utf8LanguageTagIsCjk("kor"));

  EXPECT_FALSE(utf8LanguageTagIsCjk(""));
  EXPECT_FALSE(utf8LanguageTagIsCjk("en"));
  EXPECT_FALSE(utf8LanguageTagIsCjk("de-DE"));
}
