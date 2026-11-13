#include <Dictionary.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>

namespace {
using namespace dictionary_test;
void be32(std::string& data, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) data.push_back(static_cast<char>(value >> shift));
}
void le32(std::string& data, uint32_t value) {
  for (int shift = 0; shift <= 24; shift += 8) data.push_back(static_cast<char>(value >> shift));
}
class DictionaryLookup : public ::testing::Test {
 protected:
  void SetUp() override {
    files.clear();
    readers.clear();
    bytesRead.clear();
    failRead = false;
    duplicateOpens = 0;
    Dictionary::setLookupDictPathOverride("/test");
  }
  void TearDown() override {
    Dictionary::clearLookupDictPathOverride();
    EXPECT_EQ(duplicateOpens, 0u);
    for (const auto& reader : readers) EXPECT_EQ(reader.second, 0) << reader.first;
  }
  void index(std::vector<std::string> words, const std::string& accelerator = "") {
    std::sort(words.begin(), words.end());
    std::string idx, oft(38, '\0'), cspt("CSPT\1\20\20\0", 8);
    le32(cspt, static_cast<uint32_t>((words.size() + 15) / 16));
    for (size_t i = 0; i < words.size(); ++i) {
      if (i && i % 32 == 0) le32(oft, static_cast<uint32_t>(idx.size()));
      if (i % 16 == 0) {
        std::string prefix = words[i].substr(0, 16);
        prefix.resize(16, '\0');
        cspt += prefix;
        le32(cspt, static_cast<uint32_t>(idx.size()));
      }
      idx += words[i];
      idx.push_back('\0');
      be32(idx, static_cast<uint32_t>(i));
      be32(idx, 1);
    }
    files["/test.idx"] = idx;
    files["/test.dict"] = std::string(words.size(), 'x');
    if (accelerator == "oft") files["/test.idx.oft"] = oft;
    if (accelerator == "cspt") files["/test.idx.oft.cspt"] = cspt;
    if (accelerator == "qidx") ASSERT_TRUE(Dictionary::prepareQuickIndex());
  }
  DictLocation lookup(const std::string& word) { return Dictionary::locate(Dictionary::cleanWord(word)); }
};
TEST_F(DictionaryLookup, ExactPointedWinsAndUnpointedDoesNotChooseAmbiguousPointedEntry) {
  index({"שָׁלוֹם", "שלום", "שֶׁלוֹם"});
  EXPECT_EQ(lookup("שָׁלוֹם").headword, "שָׁלוֹם");
  EXPECT_EQ(lookup("שלום").headword, "שלום");
  EXPECT_EQ(lookup("שֻׁלוֹם").headword, "שלום");
  index({"שָׁלוֹם", "שֶׁלוֹם"});
  EXPECT_FALSE(lookup("שלום").found);
  EXPECT_FALSE(lookup("שֻׁלוֹם").found);
}
TEST_F(DictionaryLookup, FallbackStripsMarksBeforeRetrimmingEdges) {
  index({"שלום", "בראשית ברא"});
  EXPECT_EQ(Dictionary::cleanWord("שָׁלוֹם"), "שָׁלוֹם");
  for (const char* query : {"שָׁלוֹם", "\u0591“שָׁלוֹם!”\u05B0", "\u05B0「שָׁלוֹם」\u0591"})
    EXPECT_EQ(lookup(query).headword, "שלום");
  EXPECT_EQ(lookup("בְּרֵאשִׁ֖ית בָּרָ֣א").headword, "בראשית ברא");
  EXPECT_FALSE(lookup("\u0591 !\u05B0").found);
  EXPECT_FALSE(lookup("").found);
}
TEST_F(DictionaryLookup, ArabicKeepsShaddahHamzaMaddahAndOtherMarks) {
  index({"شكل", "شكّل", "أ", "إ", "آ", "ا\u0654", "ا\u0655", "ا\u0653", "ش\u0656كل"});
  EXPECT_EQ(lookup("شَكَّلَ").headword, "شكّل");
  EXPECT_EQ(lookup("شَكَلَ").headword, "شكل");
  EXPECT_EQ(lookup("شَكَّٰلَ").headword, "شكّل");
  for (const char* key : {"أ", "إ", "آ", "ا\u0654", "ا\u0655", "ا\u0653", "ش\u0656كل"})
    EXPECT_EQ(lookup(std::string(key) + "\u064E").headword, key);
  index({"شكل", "ا"});
  for (const char* query : {"شَكَّلَ", "أَ", "ا\u0654َ", "ا\u0655َ", "ا\u0653َ", "ش\u0656كلَ"})
    EXPECT_FALSE(lookup(query).found) << query;
}
TEST_F(DictionaryLookup, AllSpecifiedMarksAndAdjacentCodepoints) {
  index({"אב", "شك"});
  for (uint32_t cp = 0x0590; cp <= 0x05CA; ++cp) {
    const bool removed = (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 ||
                         cp == 0x05C4 || cp == 0x05C5 || (cp >= 0x05C7 && cp <= 0x05C9);
    char mark[5] = {};
    utf8AppendCodepoint(mark, cp);
    EXPECT_EQ(lookup(std::string("א") + mark + "ב").found, removed) << cp;
  }
  EXPECT_TRUE(lookup("א\uFB1Eב").found);
  for (uint32_t cp = 0x064A; cp <= 0x0671; ++cp) {
    char mark[5] = {};
    utf8AppendCodepoint(mark, cp);
    EXPECT_EQ(lookup(std::string("ش") + mark + "ك").found,
              (cp >= 0x064B && cp <= 0x0650) || cp == 0x0652 || cp == 0x0670)
        << cp;
  }
}
TEST_F(DictionaryLookup, PreservesNfcCasePreferenceAndStemming) {
  index({"CAFÉ", "café", "run", "שלום"});
  EXPECT_EQ(lookup("“cafe\u0301!”").headword, "café");
  bool stem = true;
  EXPECT_EQ(Dictionary::locateWithStemVariants("שָׁלוֹם", &stem).headword, "שלום");
  EXPECT_FALSE(stem);
  EXPECT_EQ(Dictionary::locateWithStemVariants("running", &stem).headword, "run");
  EXPECT_TRUE(stem);
}
TEST_F(DictionaryLookup, AcceleratorBoundariesPreserveOriginalIndexOrdering) {
  for (const std::string accelerator : {"", "oft", "cspt", "qidx"}) {
    for (const size_t boundary : {16u, 32u, 256u, 512u}) {
      files.clear();
      std::vector<std::string> words;
      // Pointed entries sort BEFORE their stripped spelling. Folding the index
      // during comparisons would make the key sequence non-monotonic.
      for (size_t i = 0; i < boundary; ++i) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%04zu", i);
        words.push_back(std::string("שָ") + suffix);
      }
      words.push_back("שלום");
      words.push_back("ת");
      index(words, accelerator);
      EXPECT_EQ(lookup("שָׁלוֹם").headword, "שלום") << accelerator << boundary;
      EXPECT_EQ(lookup(words.front()).headword, words.front()) << accelerator << boundary;
      EXPECT_EQ(lookup("ת").headword, "ת") << accelerator << boundary;
      EXPECT_FALSE(lookup("תָא").found);
    }
  }
}
TEST_F(DictionaryLookup, SynonymsPreferExactThenRetryStripped) {
  index({"exact", "fallback"});
  std::string syn = "שָׁלוֹם";
  syn.push_back('\0');
  be32(syn, 0);
  syn += "שלום";
  syn.push_back('\0');
  be32(syn, 1);
  files["/test.syn"] = syn;
  EXPECT_EQ(Dictionary::resolveAltForm("שָׁלוֹם").headword, "exact");
  EXPECT_EQ(Dictionary::resolveAltForm("שֻׁלוֹם").headword, "fallback");
}
TEST_F(DictionaryLookup, PreparedPrefixTiesFindEveryHeadwordAndSynonym) {
  // Sixteen bytes includes only the first byte of the accented character.
  const std::string prefix = "abcdefghijklmnoé";
  std::vector<std::string> words;
  std::string syn, synCspt("CSPT\1\20\20\0", 8);
  le32(synCspt, 4);
  for (unsigned i = 0; i < 64; ++i) {
    char suffix[4];
    std::snprintf(suffix, sizeof(suffix), "%02u", i);
    words.push_back(prefix + suffix);
    if (i % 16 == 0) {
      synCspt += words.back().substr(0, 16);
      le32(synCspt, syn.size());
    }
    syn += words.back();
    syn.push_back('\0');
    be32(syn, 63 - i);
  }
  index(words, "cspt");
  files["/test.syn"] = syn;
  files["/test.syn.oft.cspt"] = synCspt;
  for (unsigned i = 0; i < words.size(); ++i) {
    auto direct = lookup(words[i]);
    ASSERT_TRUE(direct.found) << i;
    EXPECT_EQ(direct.offset, i);
    auto alias = Dictionary::resolveAltForm(words[i]);
    ASSERT_TRUE(alias.found) << i;
    EXPECT_EQ(alias.offset, 63 - i);
  }
}

TEST_F(DictionaryLookup, AutomaticallyIndexesAliasesAndUsesTheirExactNumberedDefinition) {
  std::vector<std::string> words(600, "same");
  index(words);
  std::string syn;
  for (unsigned i = 0; i < words.size(); ++i) {
    char word[16];
    std::snprintf(word, sizeof(word), "alias%03u", i);
    syn += word;
    syn.push_back('\0');
    be32(syn, i);
  }
  files["/test.syn"] = syn;
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  ASSERT_EQ(files.at("/test.syn.qidx").size(), 20 + 3 * 4);
  const auto savedIdx = files.at("/test.qidx");
  for (unsigned i : {0u, 255u, 256u, 511u, 512u, 599u}) {
    char word[16];
    std::snprintf(word, sizeof(word), "alias%03u", i);
    bytesRead.clear();
    auto result = Dictionary::resolveAltForm(word);
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.headword, "same");
    EXPECT_EQ(result.folderPath, "/test");
    EXPECT_EQ(result.offset, i);
    EXPECT_EQ(result.size, 1u);
    // A numbered jump reads at most one 256-entry block. Synonym bisection also probes sample words.
    EXPECT_LE(bytesRead["/test.idx"], 256u * 13);
    EXPECT_LE(bytesRead["/test.syn"], 260u * 13);
  }
  files["/test.syn"] += std::string("bad\0", 4);
  be32(files["/test.syn"], 600);
  const auto oldSynIndex = files.at("/test.syn.qidx");
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  EXPECT_NE(files.at("/test.syn.qidx"), oldSynIndex);
  EXPECT_EQ(files.at("/test.qidx"), savedIdx);
  EXPECT_FALSE(Dictionary::resolveAltForm("bad").found);
}

TEST_F(DictionaryLookup, PreparedHeadwordOffsetsDoNotSuppressSynonymIndexing) {
  index(std::vector<std::string>(64, "same"), "oft");
  files["/test.syn"] = std::string("alias\0", 6);
  be32(files["/test.syn"], 63);
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  EXPECT_TRUE(files.count("/test.syn.qidx"));
  EXPECT_FALSE(files.count("/test.qidx"));
  const auto result = Dictionary::resolveAltForm("alias");
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.offset, 63u);
}

TEST_F(DictionaryLookup, NumberedDefinitionsUse64BitIndexRecordWidth) {
  files["/test.ifo"] = "StarDict's dict ifo file\nidxoffsetbits=64\n";
  for (uint32_t i = 0; i < 258; ++i) {
    auto& idx = files["/test.idx"];
    idx += std::string("same\0", 5);
    be32(idx, i == 257 ? 1 : 0);
    be32(idx, i);
    be32(idx, 3);
  }
  files["/test.syn"] = std::string("alias\0", 6);
  be32(files["/test.syn"], 256);
  files["/test.syn"] += std::string("huge\0", 5);
  be32(files["/test.syn"], 257);
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  auto result = Dictionary::resolveAltForm("alias");
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.offset, 256u);
  EXPECT_EQ(result.size, 3u);
  EXPECT_FALSE(Dictionary::resolveAltForm("huge").found);
}

TEST_F(DictionaryLookup, IncompleteSynonymBuildLeavesHeadwordsUsableAndCanRetry) {
  index({"word"}, "qidx");
  files["/test.syn"] = "alias";
  EXPECT_FALSE(Dictionary::prepareQuickIndex());
  EXPECT_FALSE(files.count("/test.syn.qidx"));
  EXPECT_FALSE(files.count("/test.syn.qidx.tmp"));
  EXPECT_TRUE(lookup("word").found);
  files["/test.syn"].push_back('\0');
  be32(files["/test.syn"], 0);
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  EXPECT_EQ(Dictionary::resolveAltForm("alias").headword, "word");
}

TEST_F(DictionaryLookup, ReadErrorsAndCancellationDoNotBecomeFallbackHits) {
  index({"שלום"});
  failRead = true;
  const auto failed = lookup("שָׁלוֹם");
  EXPECT_TRUE(failed.readError);
  EXPECT_FALSE(failed.found);
  failRead = false;
  DictLookupCallbacks callbacks;
  callbacks.shouldCancel = [](void*) { return true; };
  EXPECT_FALSE(Dictionary::locate("שָׁלוֹם", callbacks).found);
}
}  // namespace
