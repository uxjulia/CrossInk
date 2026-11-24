#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "FsHelpers.h"
#include "TransferWriteCheckpoint.h"

namespace {

using namespace std::string_view_literals;

TEST(IsSafePathComponent, AcceptsNormalNamesAndRepeatedDots) {
  EXPECT_TRUE(FsHelpers::isSafePathComponent("volume..2.epub"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("notes...txt"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent(".hidden"sv));
  EXPECT_TRUE(FsHelpers::isSafePathComponent("book.epub"sv));
}

TEST(IsSafePathComponent, RejectsEmptyDotAndPathSeparators) {
  EXPECT_FALSE(FsHelpers::isSafePathComponent(""sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("."sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent(".."sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a/b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("a\\b"sv));
  EXPECT_FALSE(FsHelpers::isSafePathComponent("../x"sv));
}

TEST(NormalisePath, CollapsesParentReferenceWithinPath) {
  EXPECT_EQ(FsHelpers::normalisePath("/Books/../.crosspoint/x"), ".crosspoint/x");
}

TEST(NormalisePath, DropsLeadingParentReferencesPastRoot) { EXPECT_EQ(FsHelpers::normalisePath("/../../etc"), "etc"); }

// Sanitizes into a buffer of `size` bytes (at most 64, the size ScreenshotUtil uses).
std::string sanitize(const char* input, const size_t size = 64) {
  char out[64];
  FsHelpers::sanitizePathComponentForFat32(input, out, size);
  return out;
}

// Book titles from the EPUBs attached to #2103 and #2199.
constexpr char kTitle2103[] = "Богиня глюкозы. Нормализуйте уровень сахара в крови, чтобы изменить свою жизнь";
constexpr char kTitle2199[] = "Вглядываясь в солнце. Жизнь без страха смерти";

TEST(SanitizePathComponentForFat32, KeepsTitleThatFits) {
  EXPECT_EQ(sanitize("Эдем (полный перевод)"), "Эдем-(полный-перевод)");
}

// The readers copy the title into ScreenshotInfo::title (char[64]) with snprintf, which can
// end the copy partway through a Cyrillic letter.
TEST(SanitizePathComponentForFat32, DropsLetterCutOffByCaller) {
  char title[64];
  snprintf(title, sizeof(title), "%s", kTitle2103);
  EXPECT_EQ(sanitize(title), "Богиня-глюкозы.-Нормализуйте-уров");
  snprintf(title, sizeof(title), "%s", kTitle2199);
  EXPECT_EQ(sanitize(title), "Вглядываясь-в-солнце.-Жизнь-без-ст");
}

TEST(SanitizePathComponentForFat32, DoesNotSplitLetterAtBufferLimit) {
  // Each letter of "Жизнь" is 2 bytes. 7 bytes of room holds "Жиз" and half of "н".
  EXPECT_EQ(sanitize("Жизнь", 8), "Жиз");
  EXPECT_EQ(sanitize(kTitle2103), "Богиня-глюкозы.-Нормализуйте-уров");
}

TEST(TransferWriteCheckpoint, SyncsACompleteNetworkBlockBeforeReturning) {
  HalFile file;
  file.open_ = true;
  const std::vector<uint8_t> block(1024, 0x5a);

  ASSERT_TRUE(FsHelpers::writeAndCheckpoint(file, block.data(), block.size()));

  ASSERT_EQ(file.writeSizes.size(), 1u);
  EXPECT_EQ(file.writeSizes[0], block.size());
  ASSERT_EQ(file.syncSnapshots.size(), 1u);
  EXPECT_EQ(file.syncSnapshots[0], block);
  EXPECT_EQ(file.durable, block);
}

TEST(TransferWriteCheckpoint, FirstSyncFailureStopsBeforeWritingRestOfBlock) {
  HalFile file;
  file.open_ = true;
  file.failSyncAt = 1;
  const std::vector<uint8_t> block(1024, 0x42);
  const std::string existingDestination = "previous book";

  EXPECT_FALSE(FsHelpers::writeAndCheckpoint(file, block.data(), block.size()));

  ASSERT_EQ(file.writeSizes.size(), 1u);
  EXPECT_EQ(file.writeSizes[0], block.size());
  EXPECT_EQ(file.durable.size(), 0u);
  EXPECT_EQ(existingDestination, "previous book");
}

TEST(TransferWriteCheckpoint, LaterSyncFailureRetainsOnlyTheSyncedPrefix) {
  HalFile file;
  file.open_ = true;
  const std::vector<uint8_t> firstBlock(512, 0x33);
  const std::vector<uint8_t> secondBlock(512, 0x44);
  const std::string existingDestination = "previous book";

  ASSERT_TRUE(FsHelpers::writeAndCheckpoint(file, firstBlock.data(), firstBlock.size()));
  file.failSyncAt = 2;
  EXPECT_FALSE(FsHelpers::writeAndCheckpoint(file, secondBlock.data(), secondBlock.size()));

  ASSERT_EQ(file.writeSizes.size(), 2u);
  ASSERT_EQ(file.durable.size(), 512u);
  EXPECT_EQ(file.durable, std::vector<uint8_t>(512, 0x33));
  EXPECT_EQ(existingDestination, "previous book");
}

TEST(TransferWriteCheckpoint, ShortWritesAreSyncedButReportedAsFailures) {
  const std::vector<uint8_t> block(1024, 0x24);
  HalFile file;
  file.open_ = true;
  file.shortWriteAt = 1;
  const std::string existingDestination = "previous book";

  EXPECT_FALSE(FsHelpers::writeAndCheckpoint(file, block.data(), block.size()));

  EXPECT_EQ(file.writeCalls, 1u);
  EXPECT_EQ(file.durable.size(), block.size() - 1);
  EXPECT_EQ(file.syncCalls, 1u);
  EXPECT_EQ(existingDestination, "previous book");
}

TEST(TransferWriteCheckpoint, ResumeAndZeroLengthWritesKeepTheirExistingSemantics) {
  HalFile resumed;
  resumed.open_ = true;
  resumed.working.assign(10, 0x11);
  resumed.durable = resumed.working;
  resumed.cursor = resumed.working.size();
  const std::vector<uint8_t> append(1024, 0x22);
  ASSERT_TRUE(FsHelpers::writeAndCheckpoint(resumed, append.data(), append.size()));
  ASSERT_EQ(resumed.writeSizes.size(), 1u);
  EXPECT_EQ(resumed.writeSizes[0], append.size());
  ASSERT_EQ(resumed.durable.size(), 1034u);
  EXPECT_EQ(resumed.durable[9], 0x11);
  EXPECT_EQ(resumed.durable[10], 0x22);

  HalFile empty;
  empty.open_ = true;
  EXPECT_TRUE(FsHelpers::writeAndCheckpoint(empty, nullptr, 0));
  EXPECT_EQ(empty.writeCalls, 0u);
  EXPECT_EQ(empty.syncCalls, 0u);
}

}  // namespace
