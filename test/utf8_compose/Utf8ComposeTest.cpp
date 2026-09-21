#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Utf8.h"

namespace {

// Helpers to build NFD / expected byte sequences explicitly so the test does not
// depend on the encoding of this source file.
const std::string kCombGrave = "\xCC\x80";     // U+0300 COMBINING GRAVE ACCENT
const std::string kCombAcute = "\xCC\x81";     // U+0301 COMBINING ACUTE ACCENT
const std::string kCombCirc = "\xCC\x82";      // U+0302 COMBINING CIRCUMFLEX ACCENT
const std::string kCombDotBelow = "\xCC\xA3";  // U+0323 COMBINING DOT BELOW

}  // namespace

class Utf8ComposeNfc : public testing::TestWithParam<bool> {
 protected:
  std::string compose(const std::string& input) {
    if (!GetParam()) return utf8ComposeNfc(input);
    std::string buffer = input;
    utf8ComposeNfcInPlace(buffer.data());
    buffer.resize(strlen(buffer.c_str()));
    return buffer;
  }
};

INSTANTIATE_TEST_SUITE_P(StringAndInPlace, Utf8ComposeNfc, testing::Bool());

// ASCII and already-precomposed (NFC) text must pass through untouched (fast path).
TEST_P(Utf8ComposeNfc, PassesThroughAsciiAndNfc) {
  EXPECT_EQ(compose(""), "");
  EXPECT_EQ(compose("hello world"), "hello world");
  EXPECT_EQ(compose("caf\xC3\xA9"), "caf\xC3\xA9");  // é already U+00E9
}

// Single combining mark composes onto its base letter.
TEST_P(Utf8ComposeNfc, ComposesSingleMark) {
  EXPECT_EQ(compose("e" + kCombAcute), "\xC3\xA9");  // e + ́  -> é  (U+00E9)
  EXPECT_EQ(compose("a" + kCombGrave), "\xC3\xA0");  // a + ̀  -> à  (U+00E0)
}

// Vietnamese letters carry two stacked marks; composition must accumulate them
// onto the intermediate precomposed character (this is the crux of the feature).
TEST_P(Utf8ComposeNfc, ComposesStackedVietnameseMarks) {
  // a + circumflex + acute -> ấ (U+1EA5)
  EXPECT_EQ(compose("a" + kCombCirc + kCombAcute), "\xE1\xBA\xA5");
  // a + dot-below + circumflex (canonical order) -> ậ (U+1EAD)
  EXPECT_EQ(compose("a" + kCombDotBelow + kCombCirc), "\xE1\xBA\xAD");
}

// A combining mark with no composition for its base is left unchanged, and the
// base is preserved.
TEST_P(Utf8ComposeNfc, LeavesUncomposableMarksIntact) {
  const std::string in = "q" + kCombAcute;  // no precomposed "q with acute"
  EXPECT_EQ(compose(in), in);
}

// A leading combining mark (no preceding base) is emitted unchanged.
TEST_P(Utf8ComposeNfc, HandlesLeadingMark) { EXPECT_EQ(compose(kCombAcute), kCombAcute); }

// Marks embedded in a longer word compose while surrounding text is preserved.
TEST_P(Utf8ComposeNfc, ComposesWithinWord) {
  // "Ti" + e+circ+acute + "ng" -> "Tiếng"
  EXPECT_EQ(compose("Ti" + std::string("e") + kCombCirc + kCombAcute + "ng"), "Ti\xE1\xBA\xBFng");
}

// --- Hangul LV / LVT composition (macOS writes filenames in NFD) ---

namespace {
const std::string kJamoG = "\xE1\x84\x80";   // U+1100 CHOSEONG KIYEOK (leading ㄱ)
const std::string kJamoH = "\xE1\x84\x92";   // U+1112 CHOSEONG HIEUH (leading ㅎ)
const std::string kJamoA = "\xE1\x85\xA1";   // U+1161 JUNGSEONG A (medial ㅏ)
const std::string kJamoN = "\xE1\x86\xAB";   // U+11AB JONGSEONG NIEUN (trailing ㄴ)
const std::string kJamoNg = "\xE1\x86\xBC";  // U+11BC JONGSEONG IEUNG (trailing ㅇ)
}  // namespace

TEST_P(Utf8ComposeNfc, ComposesHangulLv) {
  EXPECT_EQ(compose(kJamoH + kJamoA), "\xED\x95\x98");  // ㅎ(U+1112) + ㅏ(U+1161) -> 하 (U+D558)
}

TEST_P(Utf8ComposeNfc, ComposesHangulLvt) {
  EXPECT_EQ(compose(kJamoH + kJamoA + kJamoN), "\xED\x95\x9C");  // 한 (U+D55C)
}

TEST_P(Utf8ComposeNfc, ComposesHangulWord) {
  const std::string nfd = kJamoH + kJamoA + kJamoN + kJamoG + kJamoA + kJamoNg;
  EXPECT_EQ(compose(nfd), "\xED\x95\x9C\xEA\xB0\x95");  // 한강
}

TEST_P(Utf8ComposeNfc, ComposesRealNfdFilename) {
  const std::string nfd =
      "\xE1\x84\x92\xE1\x85\xA2\xE1\x84\x8F\xE1\x85\xA5\xE1\x84\x8B\xE1\x85\xAA"
      " "
      "\xE1\x84\x92\xE1\x85\xAA\xE1\x84\x80\xE1\x85\xA1"
      ".epub";
  EXPECT_EQ(compose(nfd), "\xED\x95\xB4\xEC\xBB\xA4\xEC\x99\x80 \xED\x99\x94\xEA\xB0\x80.epub");
}

TEST_P(Utf8ComposeNfc, LeavesUncomposableJamoIntact) {
  EXPECT_EQ(compose(kJamoH), kJamoH);
  EXPECT_EQ(compose(kJamoA + kJamoN), kJamoA + kJamoN);
  EXPECT_EQ(compose(kJamoH + "a"), kJamoH + "a");
}

TEST_P(Utf8ComposeNfc, PassesThroughPrecomposedHangul) {
  EXPECT_EQ(compose("\xED\x95\x9C\xEA\xB0\x95"), "\xED\x95\x9C\xEA\xB0\x95");  // 한강
}

TEST_P(Utf8ComposeNfc, LeavesOtherE1ScriptsIntact) {
  const std::string georgian = "\xE1\x83\xA5\xE1\x83\x90";  // ქა
  EXPECT_EQ(compose(georgian), georgian);
  const std::string cherokee = "\xE1\x8F\xA3";  // Ꮳ
  EXPECT_EQ(compose(cherokee), cherokee);
}

TEST_P(Utf8ComposeNfc, ComposesFolderDisplayAndPreservesDelimiters) {
  EXPECT_EQ(compose("[" + kJamoH + kJamoA + kJamoN + "]"), "[\xED\x95\x9C]");
  EXPECT_EQ(compose("[cafe" + kCombAcute + "]"), "[caf\xC3\xA9]");
}

TEST_P(Utf8ComposeNfc, DoesNotComposeAcrossUncomposableMark) {
  EXPECT_EQ(compose(kJamoH + kCombAcute + kJamoA), kJamoH + kCombAcute + kJamoA);
}

TEST_P(Utf8ComposeNfc, PreservesFourByteCodepointsAfterComposition) {
  EXPECT_EQ(compose("e" + kCombAcute + "\xF0\x9F\x93\x96.epub"), "\xC3\xA9\xF0\x9F\x93\x96.epub");
}

TEST(Utf8ComposeNfcInPlace, FitsWithinOriginalBuffer) {
  char buffer[] = {'a', '\xCC', '\x82', '\xCC', '\x81', '\0', '!'};
  utf8ComposeNfcInPlace(buffer);
  EXPECT_STREQ(buffer, "\xE1\xBA\xA5");
  EXPECT_EQ(buffer[5], '\0');
  EXPECT_EQ(buffer[6], '!');
}

TEST(Utf8ComposeNfcInPlace, PreservesMalformedAndTruncatedBytes) {
  char buffer[] =
      "\xFF"
      "e\xCC\x81"
      "\xE1\x84";
  utf8ComposeNfcInPlace(buffer);
  EXPECT_STREQ(buffer, "\xFF\xC3\xA9\xE1\x84");
}

TEST(Utf8CjkWordCharacter, AcceptsLettersAndRejectsStandalonePunctuation) {
  EXPECT_TRUE(utf8IsCjkWordCharacter(0xAC00));   // 가 (Hangul syllable)
  EXPECT_TRUE(utf8IsCjkWordCharacter(0x4E00));   // 一 (CJK ideograph)
  EXPECT_TRUE(utf8IsCjkWordCharacter(0x3042));   // あ (Hiragana)
  EXPECT_TRUE(utf8IsCjkWordCharacter(0xFF21));   // Ａ (fullwidth Latin uppercase)
  EXPECT_FALSE(utf8IsCjkWordCharacter(0x3002));  // 。 (ideographic full stop)
  EXPECT_FALSE(utf8IsCjkWordCharacter(0xFF0C));  // ， (fullwidth comma)
}

TEST(Utf8LookupWord, RecognizesNonLatinScripts) {
  EXPECT_TRUE(utf8ContainsLookupCharacter("\xE4\xB8\xAD\xE6\x96\x87"));                          // Chinese
  EXPECT_TRUE(utf8ContainsLookupCharacter("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));  // Russian
  EXPECT_TRUE(utf8ContainsLookupCharacter("\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9"));
  EXPECT_FALSE(utf8ContainsLookupCharacter("\xE2\x80\x9C\xE2\x80\x9D"));  // smart quotes
  EXPECT_FALSE(utf8ContainsLookupCharacter("\xF0\x9F\x98\x80"));          // emoji
}

TEST(Utf8LookupWord, TrimsUnicodePunctuationWithoutDamagingWords) {
  EXPECT_EQ(utf8CleanLookupWord("\xE2\x80\x9C"
                                "caf\xC3\xA9"
                                "\xE2\x80\x9D"),
            "caf\xC3\xA9");
  EXPECT_EQ(utf8CleanLookupWord("("
                                "\xC3\xA9l\xC3\xA8ve"
                                ")"),
            "\xC3\xA9l\xC3\xA8ve");
  EXPECT_EQ(utf8CleanLookupWord("\xC2\xA1hola!"), "hola");
  EXPECT_EQ(utf8CleanLookupWord("\xE4\xB8\xAD\xE6\x96\x87\xE3\x80\x82"), "\xE4\xB8\xAD\xE6\x96\x87");
  EXPECT_EQ(utf8CleanLookupWord("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82!"),
            "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
  EXPECT_EQ(utf8CleanLookupWord("l'ete"), "l'ete");
  EXPECT_EQ(utf8CleanLookupWord("mother-in-law"), "mother-in-law");
  EXPECT_EQ(utf8CleanLookupWord("..."), "");
}

TEST(Utf8LookupWord, ComposesRetainedCombiningMarks) {
  EXPECT_EQ(utf8CleanLookupWord("(cafe" + kCombAcute + ")"), "caf\xC3\xA9");
}
