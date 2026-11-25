#include <gtest/gtest.h>

#include "PixelCache.h"
TEST(PixelCache, PaddedDecodeBlockCachesShortImage) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/short.pxc", 480, 67, 0, 0, 121));
  EXPECT_EQ(cache.bandRows, 67);
  std::memset(cache.buffer, 0x55, 120 * 67);
  ASSERT_TRUE(cache.finalize());
  const auto& bytes = Storage.bytes("/short.pxc");
  ASSERT_EQ(bytes.size(), 4u + 120u * 67u);
  EXPECT_EQ(bytes[4], 0x55);
  EXPECT_EQ(bytes.back(), 0x55);
}
TEST(PixelCache, RejectsDecodeBandAboveMemoryCeiling) {
  Storage.reset();
  PixelCache cache;
  EXPECT_FALSE(cache.begin("/large.pxc", 480, 1000, 0, 0, 300));
  EXPECT_FALSE(Storage.exists("/large.pxc"));
}

TEST(PixelCache, FailedCloseDiscardsIncompleteImage) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/failed.pxc", 480, 67, 0, 0, 121));
  Storage.failClose("/failed.pxc");
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists("/failed.pxc"));
}

TEST(PixelCache, BatchesScanlinesAndPreservesEveryPayloadByte) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/image.pxc", 464, 618, 0, 0, 1));
  std::vector<uint8_t> expected{0xd0, 1, 0x6a, 2};
  for (int y = 0; y < cache.height; ++y) {
    ASSERT_TRUE(cache.advanceTo(y));
    const auto shade = static_cast<uint8_t>(y);
    memset(cache.buffer + (y - cache.bandStart) * cache.bytesPerRow, shade, cache.bytesPerRow);
    expected.insert(expected.end(), cache.bytesPerRow, shade);
  }
  ASSERT_TRUE(cache.finalize());
  EXPECT_EQ(Storage.bytes("/image.pxc"), expected);
  EXPECT_LE(Storage.writeCalls("/image.pxc"), 2 + 39u);
}

TEST(PixelCache, KeepsWholeJpegBlocksInBandAndZeroFillsUncoveredTail) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/block.pxc", 4, 64, 0, 0, 8));
  std::vector<uint8_t> expected(68);
  expected[0] = 4;
  expected[2] = 64;
  for (int y : {0, 8, 16, 27}) {
    ASSERT_TRUE(cache.advanceTo(y));
    ASSERT_LE(y - cache.bandStart + 8, cache.bandRows);
    for (int r = 0; r < 8; ++r) {
      cache.buffer[y - cache.bandStart + r] = expected[4 + y + r] = y + r + 1;
    }
  }
  ASSERT_TRUE(cache.finalize());
  EXPECT_EQ(Storage.bytes("/block.pxc"), expected);
}
