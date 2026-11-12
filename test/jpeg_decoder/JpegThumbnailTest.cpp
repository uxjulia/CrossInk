#include <JpegToBmpConverter.h>
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <vector>

namespace {
class JpegThumbnailTest : public ::testing::TestWithParam<const char*> {};

TEST_P(JpegThumbnailTest, BothCarouselOutputsMatchIndependentDecodes) {
  std::ifstream input(std::string(JPEG_FIXTURE_DIR) + "/" + GetParam() + ".jpg", std::ios::binary);
  ASSERT_TRUE(input.is_open());
  Storage.reset();
  Storage.put("cover", {std::istreambuf_iterator<char>(input), {}});
  FsFile jpeg, center, side;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "cover", jpeg));
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "center", center));
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "side", side));
  ASSERT_TRUE(JpegToBmpConverter::jpegFileToThumbnailPair(jpeg, {center, 296, 468}, {side, 200, 390}));
  center.close();
  side.close();
  const auto pairReads = Storage.readCalls("cover");
  FsFile single;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "single", single));
  ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpeg, single, 296, 468));
  single.close();
  EXPECT_EQ(Storage.readCalls("cover") - pairReads, pairReads);
  EXPECT_EQ(Storage.bytes("center"), Storage.bytes("single"));
  EXPECT_EQ(Storage.bytes("center").size(), 62u + 40u * 468);
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "single", single));
  ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpeg, single, 200, 390));
  single.close();
  EXPECT_EQ(Storage.bytes("side"), Storage.bytes("single"));
  EXPECT_EQ(Storage.bytes("side").size(), 62u + 28u * 390);
  jpeg.close();
}

TEST(JpegThumbnailWrites, ReportsFailureWhenSecondOutputRunsOutOfSpace) {
  std::ifstream input(std::string(JPEG_FIXTURE_DIR) + "/baseline-422.jpg", std::ios::binary);
  ASSERT_TRUE(input.is_open());
  Storage.reset();
  Storage.put("cover", {std::istreambuf_iterator<char>(input), {}});
  FsFile jpeg, center, side;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "cover", jpeg));
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "center", center));
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "side", side));
  Storage.failWritesAt("side", 100);
  EXPECT_FALSE(JpegToBmpConverter::jpegFileToThumbnailPair(jpeg, {center, 296, 468}, {side, 200, 390}));
  center.close();
  side.close();
  jpeg.close();
}

INSTANTIATE_TEST_SUITE_P(Covers, JpegThumbnailTest,
                         ::testing::Values("baseline-422", "interleaved-420", "luma-422-wide"));
}  // namespace
