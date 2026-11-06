#include <Arduino.h>
#include <CssParser.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

struct CssArenaBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  void createCache() {
    const std::string text =
        "p { text-align: center; margin-top: 12px; font-size: 150%; } .em { font-weight: bold; } div p { font-style: "
        "italic; } "
        ".plain { list-style-type: none; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    ASSERT_TRUE(css.saveToCache());
  }
  void checkStyle(CssParser& css) {
    EXPECT_FALSE(css.empty());
    EXPECT_EQ(css.ruleCount(), 3u);
    const auto style = css.resolveStyle("p", "em", {{0, "div", ""}});
    EXPECT_TRUE(style.hasFontWeight());
    EXPECT_TRUE(style.hasFontStyle());
    EXPECT_TRUE(style.hasTextAlign());
    EXPECT_TRUE(style.hasMarginTop());
    EXPECT_EQ(style.marginTop.value, 12);
    ASSERT_TRUE(style.hasFontSize());
    EXPECT_EQ(style.fontSize.value, 150);
    EXPECT_EQ(style.fontSize.unit, CssUnit::Percent);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    EXPECT_EQ(style.textAlign, CssTextAlign::Center);
    const auto listStyle = css.resolveStyle("ol", "plain");
    EXPECT_TRUE(listStyle.hasListStyleType());
    EXPECT_EQ(listStyle.listStyleType, CssListStyleType::None);
  }
};
TEST_F(CssArenaBackingTest, IdenticalStylesForDefaultExternalAndDiskFallback) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    createCache();
    CssParser css("book");
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    if (mode == 2)
      EXPECT_TRUE(fakeheap::live.empty());
    else {
      ASSERT_EQ(fakeheap::live.size(), 1u);
      EXPECT_EQ(fakeheap::live.begin()->second.external, mode == 1);
    }
    css.clear();
    EXPECT_TRUE(css.empty());
    EXPECT_EQ(css.ruleCount(), 0u);
  }
}
TEST_F(CssArenaBackingTest, ExternalHydrationPreservesInternalAndExternalReserves) {
  createCache();
  for (bool internalPressure : {false, true}) {
    fakeheap::reset();
    CssParser css("book");
    if (internalPressure)
      fakeheap::internal.free = 79 * 1024;
    else
      fakeheap::external.free = 128 * 1024;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    EXPECT_TRUE(fakeheap::live.empty());
  }
}

TEST_F(CssArenaBackingTest, LargerSourceAllowanceRequiresPsram) {
  EXPECT_EQ(CssParser::maxSourceBytes(), 512u * 1024u);
  fakeheap::reset(false);
  EXPECT_EQ(CssParser::maxSourceBytes(), 128u * 1024u);
}

TEST_F(CssArenaBackingTest, LargeStreamPreservesHiddenRulesThroughCache) {
  // Publisher comments can make a stylesheet exceed the old 128 KiB limit
  // without needing a large rule map. The parser must reach the hidden rule.
  const std::string text = "/*" + std::string(191327, ' ') + "*/\n.modal { display: none; }";
  ASSERT_LT(text.size(), CssParser::maxSourceBytes());
  Storage.put("large.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "large.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
}

TEST_F(CssArenaBackingTest, PreviousCacheVersionIsInvalidated) {
  CssParser css("book");
  ASSERT_TRUE(css.saveToCache());
  FsFile file;
  // Obtain a valid empty cache, then mark it as the prior cache revision.
  ASSERT_TRUE(Storage.openFileForRead("test", "book/css_rules.cache", file));
  std::vector<uint8_t> bytes(file.size());
  ASSERT_EQ(file.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  file.close();
  bytes[4] = CssParser::CSS_CACHE_VERSION - 1;
  Storage.put("book/css_rules.cache", bytes);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssArenaBackingTest, PsramParsingMergesSelectorsAndReleasesAllSlabs) {
  const std::string text = ".modal { display: none; } .MODAL { font-weight: bold; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.ruleCount(), 1u);
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  EXPECT_EQ(css.resolveStyle("div", "modal").fontWeight, CssFontWeight::Bold);
  ASSERT_FALSE(fakeheap::live.empty());
  for (const auto& allocation : fakeheap::live) EXPECT_TRUE(allocation.second.external);
  css.clear();
  EXPECT_TRUE(fakeheap::live.empty());
}

TEST_F(CssArenaBackingTest, PsramParsingAllocationFailureStopsSafely) {
  const std::string text = ".modal { display: none; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  fakeheap::external.fail = 1;
  EXPECT_FALSE(css.loadFromStream(file));
  file.close();
  EXPECT_TRUE(css.empty());
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(fakeheap::internal.attempts, 0u);
}

TEST_F(CssArenaBackingTest, ManyRulesSurviveArenaGrowthAndCacheRoundTrip) {
  std::string text;
  for (int i = 0; i < 1303; ++i) text += ".rule" + std::to_string(i) + " { display: none; }\n";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
}

TEST_F(CssArenaBackingTest, BorderSuppressionSurvivesHydrationAndDiskFallback) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    const std::string text =
        "hr.transition { border: none; } hr.visible { border: none; border-top: 1px solid; } "
        "div hr { border-width: 0; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    EXPECT_TRUE(css.resolveStyle("hr", "transition").suppressesHorizontalRule());
    EXPECT_FALSE(css.resolveStyle("hr", "visible").suppressesHorizontalRule());
    ASSERT_TRUE(css.saveToCache());
    css.clear();
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    if (mode == 2) EXPECT_TRUE(fakeheap::live.empty());
    EXPECT_TRUE(css.resolveStyle("hr", "transition").suppressesHorizontalRule());
    EXPECT_FALSE(css.resolveStyle("hr", "visible").suppressesHorizontalRule());
    EXPECT_TRUE(css.resolveStyle("hr", "", {{0, "div", ""}}).suppressesHorizontalRule());
  }
}

TEST_F(CssArenaBackingTest, PublisherDecorationsAndContextSurviveAllCacheBackings) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    const std::string text =
        "section#chapter > p.note.wide { border: 2px dashed; background: #ddd; white-space: pre-wrap; } "
        "p.note::first-letter { initial-letter: 3; float: left; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    ASSERT_TRUE(css.saveToCache());
    css.clear();
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    auto style = css.resolveStyle("p", "wide note", {{0, "section", "", "chapter"}});
    EXPECT_EQ(style.borders[0].width, 2);
    EXPECT_EQ(style.borders[0].style, CssBorderStyle::Dashed);
    EXPECT_TRUE(style.shaded);
    EXPECT_TRUE(style.preserveWhitespace);
    auto cap = css.resolveStyle("p", "note", {}, {}, true);
    EXPECT_EQ(cap.initialLetter, 3);
    EXPECT_TRUE(cap.floatLeft);
  }
}

TEST_F(CssArenaBackingTest, PublisherFacesShareTheRulesCacheAndDetectTruncation) {
  const std::string source =
      "@font-face {font-family: Body; src:url(../font.ttf);font-weight:bold} p {font-family:Body}";
  Storage.put("input.css", {source.begin(), source.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file, "EPUB/css/"));
  file.close();
  ASSERT_TRUE(css.saveToCache());
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Complete);
  std::vector<CssFontFace> faces;
  ASSERT_TRUE(CssParser::loadFontFaces("book", faces));
  ASSERT_EQ(faces.size(), 1u);
  EXPECT_EQ(faces[0].family, CssParser::hashFontFamily("Body"));
  EXPECT_EQ(faces[0].href, "EPUB/css/../font.ttf");
  EXPECT_TRUE(faces[0].bold);
  EXPECT_TRUE(css.loadFromCache());
  Storage.data("book/css_rules.cache").bytes.pop_back();
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
  EXPECT_FALSE(CssParser::loadFontFaces("book", faces));
  EXPECT_TRUE(faces.empty());
}

TEST_F(CssArenaBackingTest, EmptyPublisherManifestIsValidAndRequired) {
  CssParser css("book");
  ASSERT_TRUE(css.saveToCache());
  std::vector<CssFontFace> faces;
  ASSERT_TRUE(CssParser::loadFontFaces("book", faces));
  EXPECT_TRUE(faces.empty());
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Complete);
  auto& bytes = Storage.data("book/css_rules.cache").bytes;
  bytes.resize(bytes.size() - 5);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssArenaBackingTest, PublisherMetadataRespectsInternalReserve) {
  fakeheap::reset(false);
  fakeheap::internal.free = 64 * 1024;
  const std::string source = "@font-face {font-family: Body; src:url(font.ttf)}";
  Storage.put("input.css", {source.begin(), source.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  EXPECT_FALSE(css.loadFromStream(file));
  file.close();
  ASSERT_TRUE(css.saveToCache());
  std::vector<CssFontFace> faces;
  ASSERT_TRUE(CssParser::loadFontFaces("book", faces));
  EXPECT_TRUE(faces.empty());
}

TEST_F(CssArenaBackingTest, BitmapSizeHintsPreferPublisherHeadingsWithoutHydratingRules) {
  std::string text = "h1 { font-size: 1.33em; } h2 { font-size: 117%; } p { font-size: 1em; }";
  for (unsigned i = 0; i < 100; ++i) text += ".note" + std::to_string(i) + " { font-size: .75em; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  {
    CssParser writer("book");
    ASSERT_TRUE(writer.loadFromStream(file));
    file.close();
    ASSERT_TRUE(writer.saveToCache());
  }
  fakeheap::reset(false);
  fakeheap::internal.fail = 1;  // No arena allocation is allowed for the summary.
  CssParser css("book");
  const auto sizes = css.contentFontSizeScales();
  EXPECT_EQ(sizes, (std::array<uint16_t, 2>{340, 300}));
  EXPECT_TRUE(css.empty());
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(fakeheap::internal.fail, 1u);
  EXPECT_EQ(Storage.openHandles("book/css_rules.cache"), 0);
  Storage.data("book/css_rules.cache").bytes[5] = 1;  // A valid partial cache is useful too.
  EXPECT_EQ(css.contentFontSizeScales(), sizes);
  // This is the supplied EPUB's 10pt case: nearest files are 13pt and 12pt.
  EXPECT_EQ((10 * sizes[0] + 128) / 256, 13);
  EXPECT_EQ((10 * sizes[1] + 128) / 256, 12);
}

TEST_F(CssArenaBackingTest, BitmapSizeHintsSupportUnitsDescendantsAndStableOrdering) {
  const std::string text = "section h1.title { font-size: 24px; } h2 { font-size: 15pt; } p { font-size: 100%; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  EXPECT_EQ(css.contentFontSizeScales(), (std::array<uint16_t, 2>{384, 320}));
  EXPECT_EQ(css.contentFontSizeScales(), (std::array<uint16_t, 2>{384, 320}));
}

TEST_F(CssArenaBackingTest, BitmapSizeHintsFallBackOnMissingOrInterruptedCache) {
  CssParser css("book");
  const std::array<uint16_t, 2> defaults{512, 192};
  EXPECT_EQ(css.contentFontSizeScales(), defaults);
  createCache();
  const auto original = Storage.data("book/css_rules.cache").bytes;
  for (size_t length = 0; length < original.size(); ++length) {
    Storage.put("book/css_rules.cache", {original.begin(), original.begin() + length});
    EXPECT_EQ(css.contentFontSizeScales(), defaults) << length;
  }
  Storage.put("book/css_rules.cache", original);
  Storage.data("book/css_rules.cache").bytes[4] = 0;  // Old format.
  EXPECT_EQ(css.contentFontSizeScales(), defaults);
}

TEST_F(CssArenaBackingTest, SmallPrintHintsPreserveDefaultHeadingEnlargement) {
  const std::string text = ".footnote { font-size: 75%; } .caption { font-size: 83%; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  const auto scales = css.contentFontSizeScales();
  EXPECT_LT(scales[0], 256);
  EXPECT_EQ(scales[1], 512);
}

TEST_F(CssArenaBackingTest, StylesSurviveLowHeapWithHydratedAndDiskBackedRules) {
  for (bool hydrated : {false, true}) {
    fakeheap::reset(false);
    createCache();
    {
      CssParser css("book");
      if (!hydrated) fakeheap::internal.free = 40 * 1024;
      ASSERT_TRUE(css.loadFromCache());
      for (size_t free : {size_t(48 * 1024), size_t(48 * 1024 - 1), size_t(24 * 1024)}) {
        fakeheap::internal.free = free;
        checkStyle(css);
      }
    }
    EXPECT_TRUE(fakeheap::live.empty());
  }
}
