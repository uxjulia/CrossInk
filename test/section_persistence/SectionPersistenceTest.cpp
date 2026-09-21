#include <gtest/gtest.h>

#include <cstdint>
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

namespace {
constexpr uint8_t kFullVersion = 77;
constexpr uint8_t kPartialVersion = 0xF3;
constexpr uint8_t kPreviousFullVersion = 76;
constexpr uint8_t kPreviousPartialVersion = 0xF5;

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
  Section section{epub, 0, renderer};
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
    context->parser->anchorData = anchors;
    section.build_ = std::move(context);
    ASSERT_TRUE(Storage.openFileForWrite("TEST", section.build_->tmpSectionPath, section.file));
    ASSERT_TRUE(section.writeSectionFileHeader(spec));
  }

  void appendPages(const size_t count) {
    for (size_t i = 0; i < count; ++i) {
      ASSERT_EQ(section.build_->pageIndex.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
      const uint32_t offset = section.onPageComplete(std::make_unique<Page>());
      ASSERT_NE(offset, 0U);
      section.build_->pageIndex.appendPrepared(
          {offset, static_cast<uint16_t>(i * 3U), static_cast<uint16_t>(i * 5U), static_cast<uint32_t>(i * 17U)});
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
  void SetUp() override { Storage.reset(); }
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
                                      static_cast<uint16_t>(i * 5U), static_cast<uint32_t>(i * 17U)});
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
  for (const uint8_t staleVersion : {kPreviousFullVersion, kPreviousPartialVersion}) {
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
}  // namespace
