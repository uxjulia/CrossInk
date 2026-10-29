#include <gtest/gtest.h>

// Include standard headers before the class/private macros below: libstdc++
// templates declared with `class` do not compile if first parsed under them.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define class struct
#define private public
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <GfxRenderer.h>

extern uint32_t testHyphenationIdentity;
extern std::vector<BookMetadataCache::TocEntry> testTocEntries;

namespace {
constexpr uint8_t kFullVersion = 102;
constexpr uint8_t kPreRoundingFullVersion = 100;
constexpr uint8_t kPreRoundingPartialVersion = 0xD5;
constexpr uint8_t kStablePageFullVersion = 99;
constexpr uint8_t kStablePagePartialVersion = 0xD4;
constexpr uint8_t kPreDropCapFullVersion = 98;
constexpr uint8_t kPreDropCapPartialVersion = 0xD3;
constexpr uint8_t kPreImageAlignmentFullVersion = 94;
constexpr uint8_t kPreImageAlignmentPartialVersion = 0xCF;
constexpr uint8_t kPreFieldsetFullVersion = 93;
constexpr uint8_t kPreFieldsetPartialVersion = 0xCE;
constexpr uint8_t kS3BitmapFullVersion = 90;
constexpr uint8_t kPartialVersion = 0xD7;
constexpr uint8_t kS3BitmapPartialVersion = 0xCB;
constexpr uint8_t kC3BitmapFullVersion = 89;
constexpr uint8_t kC3BitmapPartialVersion = 0xCA;
constexpr uint8_t kPreviousFullVersion = 87;
constexpr uint8_t kParagraphSpacingFullVersion = 85;
constexpr uint8_t kBorderSuppressionFullVersion = 84;
constexpr uint8_t kPreviousPartialVersion = 0xC8;
constexpr uint8_t kParagraphSpacingPartialVersion = 0xC6;
constexpr uint8_t kBorderSuppressionPartialVersion = 0xC5;
constexpr uint8_t kOlderFullVersion = 79;
constexpr uint8_t kOlderPartialVersion = 0xF4;
constexpr uint8_t kEarlierFullVersion = 78;
constexpr uint8_t kEarlierPartialVersion = 0xF2;
constexpr uint8_t kLastReleaseFullVersion = 77;
constexpr uint8_t kLastReleasePartialVersion = 0xF3;
constexpr uint8_t kPreviousReleasePrepPartialVersion = 0x80;

ReaderRenderSpec renderSpec() {
  ReaderRenderSpec spec;
  spec.fontId = 3;
  spec.viewportWidth = 480;
  spec.viewportHeight = 800;
  spec.hyphenationEnabled = true;
  spec.wordSpacing = 2;
  return spec;
}

struct SectionHarness {
  Epub epub{"/books/test.epub", "/cache"};
  GfxRenderer renderer;
  Section section;
  explicit SectionHarness(EpubRenderMode mode = EpubRenderMode::CrossInkDefault)
      : section(epub, 0, renderer, sectionCacheSuffixForRenderMode(mode)) {}
  ReaderRenderSpec spec = renderSpec();

  void begin(const std::vector<std::pair<std::string, uint16_t>>& anchors = {}) {
    auto context = makeUniqueNoThrow<Section::BuildContext>();
    ASSERT_NE(context, nullptr);
    context->tmpSectionPath = section.binTmpPath();
    context->parsePath = "/cache/test.html";
    context->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        epub, context->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
        spec.forceParagraphIndents, spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight,
        spec.hyphenationEnabled, spec.focusReadingEnabled, spec.guideReadingEnabled, spec.wordSpacing,
        [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) {}, spec.embeddedStyle, "", "",
        spec.imageRendering, std::vector<std::string>{}, nullptr, nullptr, spec.renderMode);
    ASSERT_NE(context->parser, nullptr);
    context->parser->anchorData.assign(anchors.begin(), anchors.end());
    section.build_ = std::move(context);
    ASSERT_TRUE(Storage.openFileForWrite("TEST", section.build_->tmpSectionPath, section.file));
    ASSERT_TRUE(section.writeSectionFileHeader(spec));
  }

  void appendPages(const size_t count) {
    for (size_t i = 0; i < count; ++i) {
      ASSERT_EQ(section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
      const uint32_t offset = section.onPageComplete(std::make_unique<Page>());
      ASSERT_NE(offset, 0U);
      section.build_->pageIndex.appendPrepared({offset, static_cast<uint16_t>(i * 3U), static_cast<uint16_t>(i * 5U),
                                                static_cast<uint32_t>(i * 17U), static_cast<uint32_t>(i * 13U)});
    }
  }

  bool commit(const uint8_t version, const uint32_t consumed = 0, const uint32_t total = 0) {
    return section.commitBuildFile(version, consumed, total);
  }

  void finishSuccessfulCommit() {
    section.build_.reset();
    section.buildComplete_ = true;
  }
};

class SectionPersistenceTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    testTocEntries.clear();
    testHyphenationIdentity = 1;
  }
};

TEST_F(SectionPersistenceTest, FullCommitReopensAndResolvesMetadataAcrossAChunkBoundary) {
  SectionHarness harness;
  harness.begin({{"chapter", 0}, {"boundary", 64}});
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();

  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_FALSE(reopened.isPartial());
  EXPECT_EQ(reopened.pageCount, 65);
  EXPECT_EQ(reopened.findAnchor("boundary"), 64);
  EXPECT_EQ(reopened.getParagraphIndexForPage(63), 189);
  EXPECT_EQ(reopened.getParagraphIndexForPage(64), 192);
  EXPECT_EQ(reopened.getListItemIndexForPage(64), 320);
  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(64), 1088U);
  EXPECT_EQ(reopened.getPageForParagraphIndex(192), 64);
  EXPECT_EQ(reopened.getPageForListItemIndex(320), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_NE(reopened.loadPage(64), nullptr);
}

TEST_F(SectionPersistenceTest, PartialCommitFiltersFutureAnchorsAndFallsBackBeyondTheLivePrefix) {
  SectionHarness harness;
  harness.begin({{"chapter", 0}, {"boundary", 64}, {"future", 65}});
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kPartialVersion, 12345, 67890));
  harness.finishSuccessfulCommit();

  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  ASSERT_TRUE(reopened.isPartial());
  EXPECT_EQ(reopened.pageCount, 65);
  EXPECT_EQ(reopened.findAnchor("boundary"), 64);
  EXPECT_EQ(reopened.findAnchor("future"), std::nullopt);

  auto replay = makeUniqueNoThrow<Section::BuildContext>();
  ASSERT_NE(replay, nullptr);
  for (size_t i = 0; i < 10; ++i) {
    ASSERT_EQ(replay->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    replay->pageIndex.appendPrepared({static_cast<uint32_t>(1000 + i), static_cast<uint16_t>(i * 3U),
                                      static_cast<uint16_t>(i * 5U), static_cast<uint32_t>(i * 17U),
                                      static_cast<uint32_t>(i * 13U)});
  }
  reopened.build_ = std::move(replay);
  reopened.builtPageCount_ = 10;

  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(9), 153U);
  EXPECT_EQ(reopened.getVisibleTextOffsetForPage(64), 1088U);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_EQ(reopened.getParagraphIndexForPage(64), 192);
}

TEST_F(SectionPersistenceTest, FailedCommitKeepsThePreviousReadableCache) {
  SectionHarness baseline;
  baseline.begin({{"chapter", 0}});
  baseline.appendPages(2);
  ASSERT_TRUE(baseline.commit(kFullVersion));
  baseline.finishSuccessfulCommit();
  const std::vector<uint8_t> previous = Storage.bytes(baseline.section.filePath);

  SectionHarness replacement;
  replacement.begin({{"chapter", 0}});
  replacement.appendPages(65);
  Storage.failWritesAt(replacement.section.build_->tmpSectionPath, replacement.section.file.position() + 4);
  EXPECT_FALSE(replacement.commit(kFullVersion));
  EXPECT_FALSE(Storage.exists(replacement.section.build_->tmpSectionPath.c_str()));
  ASSERT_TRUE(Storage.exists(replacement.section.filePath.c_str()));
  EXPECT_EQ(Storage.bytes(replacement.section.filePath), previous);
  replacement.section.build_.reset();
}

TEST_F(SectionPersistenceTest, RejectsCachesFromPreviousLayoutRevisions) {
  for (const uint8_t staleVersion : {uint8_t{101},
                                     uint8_t{0xD6},
                                     kPreRoundingFullVersion,
                                     kPreRoundingPartialVersion,
                                     kStablePageFullVersion,
                                     kStablePagePartialVersion,
                                     kPreDropCapFullVersion,
                                     kPreDropCapPartialVersion,
                                     uint8_t{97},
                                     uint8_t{0xD2},
                                     uint8_t{96},
                                     uint8_t{0xD1},
                                     uint8_t{95},
                                     uint8_t{0xD0},
                                     kPreImageAlignmentFullVersion,
                                     kPreImageAlignmentPartialVersion,
                                     kPreFieldsetFullVersion,
                                     kPreFieldsetPartialVersion,
                                     kS3BitmapFullVersion,
                                     kS3BitmapPartialVersion,
                                     kC3BitmapFullVersion,
                                     kC3BitmapPartialVersion,
                                     kPreviousFullVersion,
                                     kPreviousPartialVersion,
                                     kParagraphSpacingFullVersion,
                                     kParagraphSpacingPartialVersion,
                                     kBorderSuppressionFullVersion,
                                     kBorderSuppressionPartialVersion,
                                     kOlderFullVersion,
                                     kOlderPartialVersion,
                                     kEarlierFullVersion,
                                     kEarlierPartialVersion,
                                     kLastReleaseFullVersion,
                                     kLastReleasePartialVersion,
                                     kPreviousReleasePrepPartialVersion}) {
    SectionHarness harness;
    harness.begin();
    harness.appendPages(1);
    ASSERT_TRUE(harness.commit(staleVersion, 12345, 67890));
    harness.finishSuccessfulCommit();

    Section reopened(harness.epub, 0, harness.renderer);
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
    EXPECT_FALSE(Storage.exists(harness.section.filePath.c_str()));
  }
}
TEST_F(SectionPersistenceTest, PositionLookupBatchesReadsAcrossALongChapter) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(1025);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const size_t readsBefore = Storage.reads(reopened.filePath);
  const size_t seeksBefore = Storage.seeks(reopened.filePath);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1024 * 17), 1024);
  // Five header reads plus ceil(1025/32) batches, and just three seeks.
  EXPECT_LE(Storage.reads(reopened.filePath) - readsBefore, 38U);
  EXPECT_LE(Storage.seeks(reopened.filePath) - seeksBefore, 3U);
}

TEST_F(SectionPersistenceTest, PositionLookupPreservesFirstAndLastDuplicateAcrossBatchBoundary) {
  SectionHarness harness;
  harness.begin();
  for (size_t i = 0; i < 66; ++i) {
    ASSERT_EQ(harness.section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    const uint32_t position = harness.section.onPageComplete(std::make_unique<Page>());
    const uint32_t offset = i < 31 ? 0 : (i <= 64 ? 100 : 200);
    harness.section.build_->pageIndex.appendPrepared({position, 0, 0, offset});
  }
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(0, true), 0);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(0), 30);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(100, true), 31);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(100), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(150, true), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(999), 65);
}

TEST_F(SectionPersistenceTest, PartialPositionLookupRejectsOffsetsBeyondCommittedPages) {
  SectionHarness harness;
  harness.begin();
  harness.appendPages(65);
  ASSERT_TRUE(harness.commit(kPartialVersion, 12345, 67890));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1088), 64);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1089), std::nullopt);
  EXPECT_EQ(reopened.getPageForVisibleTextOffset(1089, true), std::nullopt);
}

}  // namespace

TEST_F(SectionPersistenceTest, SyncLookupsUseTheSelectedRenderModesCache) {
  const EpubRenderMode modes[] = {EpubRenderMode::CrossInkDefault, EpubRenderMode::Balanced, EpubRenderMode::Light};
  for (int i = 0; i < 3; ++i) {
    SectionHarness harness(modes[i]);
    harness.spec.renderMode = modes[i];
    harness.begin();
    harness.appendPages(i + 2);
    ASSERT_TRUE(harness.commit(kFullVersion));
    harness.finishSuccessfulCommit();
  }
  Epub epub{"/books/test.epub", "/cache"};
  GfxRenderer renderer;
  for (int i = 0; i < 3; ++i) {
    Section section(epub, 0, renderer, sectionCacheSuffixForRenderMode(modes[i]));
    EXPECT_EQ(section.getCachedPageCount(), i + 2);
    EXPECT_EQ(section.getPageForParagraphIndex((i + 1) * 3), i + 1);
  }
}

TEST_F(SectionPersistenceTest, ChangedPatternsInvalidateCompleteAndPartialSections) {
  for (const auto version : {kFullVersion, kPartialVersion}) {
    Storage.reset();
    testTocEntries.clear();
    testHyphenationIdentity = 41;
    SectionHarness harness;
    harness.begin();
    harness.appendPages(1);
    ASSERT_TRUE(harness.commit(version, version == kPartialVersion ? 100 : 0, 200));
    harness.finishSuccessfulCommit();
    testHyphenationIdentity = 42;
    Section reopened(harness.epub, 0, harness.renderer);
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
  }
}

TEST_F(SectionPersistenceTest, PatternChangesDoNotInvalidateDisabledHyphenation) {
  SectionHarness harness;
  harness.spec.hyphenationEnabled = false;
  harness.begin();
  harness.appendPages(1);
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  testHyphenationIdentity = 99;
  Section reopened(harness.epub, 0, harness.renderer);
  EXPECT_TRUE(reopened.loadSectionFile(harness.spec));
}

TEST_F(SectionPersistenceTest, CharacterSpacingInvalidatesCompleteAndPartialSections) {
  for (const auto version : {kFullVersion, kPartialVersion}) {
    Storage.reset();
    testTocEntries.clear();
    SectionHarness harness;
    harness.spec.characterSpacing = -5;
    harness.begin();
    harness.appendPages(1);
    ASSERT_TRUE(harness.commit(version, version == kPartialVersion ? 100 : 0, 200));
    harness.finishSuccessfulCommit();
    Section reopened(harness.epub, 0, harness.renderer);
    EXPECT_TRUE(reopened.loadSectionFile(harness.spec));
    harness.spec.characterSpacing = 5;
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
  }
}

TEST_F(SectionPersistenceTest, ParagraphPositionMatchesLegacyWalkAcrossChunks) {
  SectionHarness harness;
  harness.begin();
  // A long paragraph crosses both 64-entry chunk boundaries.
  for (uint16_t i = 0; i < 150; ++i) {
    ASSERT_EQ(harness.section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    const auto offset = harness.section.onPageComplete(std::make_unique<Page>());
    harness.section.build_->pageIndex.appendPrepared({offset,
                                                      static_cast<uint16_t>(i < 2     ? 1
                                                                            : i < 140 ? 7
                                                                                      : 9),
                                                      0, i});
  }
  ASSERT_TRUE(harness.commit(kFullVersion));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  const auto before = Storage.reads(reopened.filePath);
  const auto position = reopened.getParagraphPositionForPage(90);
  ASSERT_TRUE(position);
  EXPECT_EQ(position->index, 7);
  EXPECT_EQ(position->offset, 88);
  EXPECT_EQ(position->span, 138);
  EXPECT_LE(Storage.reads(reopened.filePath) - before, 6u);
  for (uint16_t page : {0, 1, 2, 63, 64, 90, 127, 128, 139, 140, 149}) {
    const auto index = reopened.getParagraphIndexForPage(page);
    ASSERT_TRUE(index);
    uint16_t start = page, end = page + 1;
    while (start > 0 && reopened.getParagraphIndexForPage(start - 1) == index) --start;
    while (end < reopened.pageCount && reopened.getParagraphIndexForPage(end) == index) ++end;
    const auto actual = reopened.getParagraphPositionForPage(page);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->index, *index);
    EXPECT_EQ(actual->offset, page - start);
    EXPECT_EQ(actual->span, end - start);
  }
  EXPECT_FALSE(reopened.getParagraphPositionForPage(150));
}

TEST_F(SectionPersistenceTest, ParagraphPositionUsesLivePrefixAndRejectsInvalidDiskTable) {
  SectionHarness harness;
  harness.begin();
  const size_t headerSize = harness.section.file.position();
  harness.appendPages(5);
  ASSERT_TRUE(harness.commit(kPartialVersion, 100, 200));
  harness.finishSuccessfulCommit();
  Section reopened(harness.epub, 0, harness.renderer);
  ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
  auto replay = makeUniqueNoThrow<Section::BuildContext>();
  for (uint16_t i = 0; i < 3; ++i) {
    ASSERT_EQ(replay->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    replay->pageIndex.appendPrepared({100u + i, 9, 0, 0});
  }
  reopened.build_ = std::move(replay);
  // Disk page 3 is also paragraph 9, but page 4 is paragraph 12.
  const auto position = reopened.getParagraphPositionForPage(1);
  ASSERT_TRUE(position);
  EXPECT_EQ(position->index, 9);
  EXPECT_EQ(position->offset, 1);
  EXPECT_EQ(position->span, 4);
  const auto diskPosition = reopened.getParagraphPositionForPage(3);
  ASSERT_TRUE(diskPosition);
  EXPECT_EQ(diskPosition->offset, 3);
  EXPECT_EQ(diskPosition->span, 4);
  reopened.build_.reset();
  auto bytes = Storage.bytes(reopened.filePath);
  const uint32_t invalid = UINT32_MAX;
  memcpy(bytes.data() + headerSize - 3 * sizeof(uint32_t), &invalid, sizeof(invalid));
  Storage.put(reopened.filePath, bytes);
  EXPECT_FALSE(reopened.getParagraphPositionForPage(1));
}

TEST_F(SectionPersistenceTest, PublisherFaceStateSurvivesCompleteAndPartialCommits) {
  for (const uint8_t version : {kFullVersion, kPartialVersion}) {
    Storage.reset();
    testTocEntries.clear();
    SectionHarness harness;
    harness.spec.publisherFonts = PublisherFonts::Accents;
    harness.begin();
    harness.renderer.publisherUsage = 1;
    harness.renderer.publisherSignature = 1111;
    harness.appendPages(1);
    const auto firstPageSignature = harness.section.layoutSignature();
    harness.renderer.publisherUsage = 3;
    harness.renderer.publisherSignature = 1234;
    harness.appendPages(1);
    EXPECT_NE(harness.section.layoutSignature(), firstPageSignature);
    // Closing the font provider before a suspended commit cannot erase page requirements.
    harness.renderer.publisherUsage = 0;
    harness.renderer.publisherSignature = 0;
    ASSERT_TRUE(harness.commit(version, 10, 100));
    harness.finishSuccessfulCommit();
    harness.renderer.publisherSignature = 1234;
    Section reopened(harness.epub, 0, harness.renderer);
    ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
    EXPECT_EQ(harness.renderer.restoredPublisherUsage, 3u);
    EXPECT_EQ(reopened.layoutSignature(), harness.section.layoutSignature());
    EXPECT_NE(reopened.layoutSignature(), readerRenderSpecSignature(harness.spec));
    EXPECT_EQ(reopened.isPartial(), version == kPartialVersion);
    harness.renderer.publisherSignature = 5678;
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
  }
}

TEST_F(SectionPersistenceTest, ContentsSelectsLatestSubchapterAcrossAnchorBatches) {
  SectionHarness h;
  std::vector<std::pair<std::string, uint16_t>> anchors;
  for (int i = 0; i < 70; ++i) {
    BookMetadataCache::TocEntry entry{};
    entry.spineIndex = 0;
    entry.anchor = "chapter" + std::to_string(i);
    anchors.emplace_back(entry.anchor, i);
    testTocEntries.push_back(entry);
  }
  h.begin(anchors);
  EXPECT_EQ(h.section.getTocIndexForPage(65), 65);
  h.appendPages(70);
  ASSERT_TRUE(h.commit(kFullVersion));
  h.finishSuccessfulCommit();
  EXPECT_EQ(h.section.getTocIndexForPage(65), 65);
  EXPECT_EQ(h.section.getTocIndexForPage(0), 0);
  EXPECT_EQ(h.section.getTocIndexForPage(100), 69);
}

TEST_F(SectionPersistenceTest, ReferenceCoordinatesSurviveCompletePartialAndLiveReplay) {
  for (const uint8_t version : {kFullVersion, kPartialVersion}) {
    Storage.reset();
    SectionHarness harness;
    harness.begin();
    harness.appendPages(65);
    harness.section.build_->parser->referenceTextOffset = 1000;
    uint32_t textLength = 0;
    EXPECT_EQ(harness.section.getReferenceTextOffsetForPage(64, textLength), 64U * 13U);
    EXPECT_EQ(textLength, 0U);
    ASSERT_TRUE(harness.commit(version, 100, 200));
    harness.finishSuccessfulCommit();
    Section reopened(harness.epub, 0, harness.renderer);
    ASSERT_TRUE(reopened.loadSectionFile(harness.spec));
    EXPECT_EQ(reopened.getReferenceTextOffsetForPage(64, textLength), 64U * 13U);
    EXPECT_EQ(textLength, version == kFullVersion ? 1000U : 0U);
    EXPECT_FALSE(reopened.getReferenceTextOffsetForPage(65, textLength));
    EXPECT_EQ(reopened.getVisibleTextOffsetForPage(64), 64U * 17U);
  }
}

TEST_F(SectionPersistenceTest, PreviousReferenceLessCachesAreRejected) {
  for (const uint8_t version : {uint8_t{98}, uint8_t{0xD3}}) {
    Storage.reset();
    SectionHarness harness;
    harness.begin();
    harness.appendPages(2);
    ASSERT_TRUE(harness.commit(version));
    harness.finishSuccessfulCommit();
    Section reopened(harness.epub, 0, harness.renderer);
    EXPECT_FALSE(reopened.loadSectionFile(harness.spec));
  }
}
