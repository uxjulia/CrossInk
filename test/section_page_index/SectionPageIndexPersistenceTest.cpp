#include <Serialization.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "SectionPageIndexSerialization.h"

namespace {
using Anchor = std::pair<std::string, uint16_t>;

SectionPageIndex makeIndex(const size_t count) {
  SectionPageIndex index;
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(index.prepareAppend(), SectionPageIndex::PrepareResult::Ready);
    index.appendPrepared({static_cast<uint32_t>(200U + i * 13U), static_cast<uint16_t>(i * 3U),
                          static_cast<uint16_t>(i * 5U), static_cast<uint32_t>(i * 17U)});
  }
  return index;
}

template <typename T>
void appendPod(std::vector<uint8_t>& output, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  output.insert(output.end(), bytes, bytes + sizeof(T));
}

void appendString(std::vector<uint8_t>& output, const std::string& value) {
  appendPod(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

std::vector<uint8_t> baselineBytes(const std::vector<uint8_t>& prefix, const SectionPageIndex& index,
                                   const std::vector<Anchor>& anchors) {
  std::vector<uint8_t> output = prefix;
  for (size_t i = 0; i < index.size(); ++i) appendPod(output, index[i].fileOffset);
  appendPod(output, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    appendString(output, anchor);
    appendPod(output, page);
  }
  appendPod(output, static_cast<uint16_t>(index.size()));
  for (size_t i = 0; i < index.size(); ++i) appendPod(output, index[i].paragraphIndex);
  for (size_t i = 0; i < index.size(); ++i) appendPod(output, index[i].listItemIndex);
  for (size_t i = 0; i < index.size(); ++i) appendPod(output, index[i].visibleTextOffset);
  for (size_t i = 0; i < index.size(); ++i) appendPod(output, index[i].referenceTextOffset);
  return output;
}

bool writeAnchors(FsFile& file, const std::vector<Anchor>& anchors) {
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) return false;
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) return false;
  }
  return true;
}

TEST(SectionPageIndexPersistence, FullCacheFieldsMatchTheContiguousBaselineAcrossAChunkBoundary) {
  auto index = makeIndex(65);
  const std::vector<Anchor> anchors{{"chapter", 0}, {"footnote", 64}};
  const std::vector<uint8_t> prefix{0xFF, 'C', 'X', 'S', 66, 1, 2, 3, 4};
  FsFile file;
  file.bytes = prefix;
  file.cursor = prefix.size();
  SectionPageIndexOffsets offsets;

  ASSERT_TRUE(writeSectionPageIndex(
      file, index, [&anchors](FsFile& output) { return writeAnchors(output, anchors); }, offsets));
  EXPECT_EQ(file.bytes, baselineBytes(prefix, index, anchors));
  EXPECT_EQ(offsets.page, prefix.size());
  EXPECT_EQ(offsets.anchorMap, prefix.size() + index.size() * sizeof(uint32_t));
  EXPECT_LT(offsets.anchorMap, offsets.paragraph);
  EXPECT_LT(offsets.paragraph, offsets.listItem);
  EXPECT_LT(offsets.listItem, offsets.visibleText);
}

TEST(SectionPageIndexPersistence, PartialCacheFiltersFutureAnchorsAndKeepsTheWatermarkTrailer) {
  auto index = makeIndex(65);
  const std::vector<Anchor> anchors{{"chapter", 0}, {"kept", 64}, {"future", 65}};
  const std::vector<Anchor> readableAnchors{{"chapter", 0}, {"kept", 64}};
  const std::vector<uint8_t> prefix{0xFF, 'C', 'X', 'S', 0xF6, 9, 8, 7};
  FsFile file;
  file.bytes = prefix;
  file.cursor = prefix.size();
  SectionPageIndexOffsets offsets;

  ASSERT_TRUE(writeSectionPageIndex(
      file, index,
      [&anchors, &index](FsFile& output) {
        std::vector<Anchor> readable;
        for (const auto& anchor : anchors) {
          if (anchor.second < index.size()) readable.push_back(anchor);
        }
        return writeAnchors(output, readable);
      },
      offsets));
  constexpr uint32_t bytesConsumed = 12345;
  constexpr uint32_t totalBytes = 67890;
  ASSERT_TRUE(serialization::tryWritePod(file, bytesConsumed));
  ASSERT_TRUE(serialization::tryWritePod(file, totalBytes));

  auto expected = baselineBytes(prefix, index, readableAnchors);
  appendPod(expected, bytesConsumed);
  appendPod(expected, totalBytes);
  EXPECT_EQ(file.bytes, expected);
}

TEST(SectionPageIndexPersistence, SerializationFailureStopsWithoutPublishingLaterArrays) {
  auto index = makeIndex(65);
  FsFile file;
  file.failAt = 64 * sizeof(uint32_t) + 2;
  SectionPageIndexOffsets offsets;

  EXPECT_FALSE(writeSectionPageIndex(
      file, index, [](FsFile& output) { return serialization::tryWritePod(output, uint16_t{0}); }, offsets));
  EXPECT_EQ(file.bytes.size(), file.failAt);
  EXPECT_EQ(offsets.anchorMap, 0U);
  EXPECT_EQ(offsets.paragraph, 0U);
}
}  // namespace
