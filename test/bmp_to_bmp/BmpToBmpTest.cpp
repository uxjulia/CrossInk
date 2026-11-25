#include <BmpToBmpConverter.h>
#include <HalStorage.h>
#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

class Output final : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t{b[at]} | uint32_t{b[at + 1]} << 8 | uint32_t{b[at + 2]} << 16 | uint32_t{b[at + 3]} << 24;
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// 24bpp or 1bpp BMP whose top (or left) half is black and the other half white.
std::vector<uint8_t> splitBmp(int width, int height, bool topDown, bool oneBit, bool leftRight = false) {
  const int bits = oneBit ? 1 : 24;
  const int rowBytes = (width * bits + 31) / 32 * 4;
  const uint32_t offset = 54 + (oneBit ? 8 : 0);
  std::vector<uint8_t> b = {'B', 'M'};
  put32(b, offset + rowBytes * height);
  put32(b, 0);
  put32(b, offset);
  put32(b, 40);
  put32(b, width);
  put32(b, topDown ? static_cast<uint32_t>(-height) : height);
  b.insert(b.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  for (int i = 0; i < 6; ++i) put32(b, i == 4 && oneBit ? 2 : 0);
  if (oneBit) {
    put32(b, 0);
    put32(b, 0xFFFFFF);
  }
  for (int i = 0; i < height; ++i) {
    const int y = topDown ? i : height - 1 - i;
    if (!leftRight) {
      b.insert(b.end(), rowBytes, y < height / 2 ? 0 : 0xFF);
      continue;
    }
    for (int x = 0; x < rowBytes; ++x) b.push_back(x < rowBytes / 2 ? 0 : 0xFF);
  }
  return b;
}

class BmpToBmpTest : public ::testing::Test {
 protected:
  Output convert(const std::vector<uint8_t>& bmp, bool crop, int width = 40, int height = 64) {
    HalFile file(bmp);
    Output out;
    EXPECT_TRUE(BmpToBmpConverter::bmpFileToBmpStream(file, out, width, height, crop, ImageToneProfile::Legacy));
    return out;
  }
  // Display stub is 64x40, so portrait output is at most 40x64. Returns level of pixel (x, y).
  static int level(const Output& out, int x, int y) {
    const unsigned bits = out.bytes[28];
    const uint32_t width = le32(out.bytes, 18);
    const auto rawHeight = static_cast<int32_t>(le32(out.bytes, 22));
    if (rawHeight > 0) y = rawHeight - 1 - y;
    const size_t row = le32(out.bytes, 10) + y * ((width * bits + 31) / 32 * 4);
    const uint8_t byte = out.bytes[row + x * bits / 8];
    return bits == 1 ? (byte >> (7 - x % 8)) & 1 : (byte >> (6 - (x % 4) * 2)) & 3;
  }
};

TEST_F(BmpToBmpTest, UpscalesSmallImageToFillScreen) {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(10, 16, topDown, false), true);
    EXPECT_EQ(le32(out.bytes, 18), 40u);
    EXPECT_EQ(le32(out.bytes, 22), topDown ? static_cast<uint32_t>(-64) : 64u);
    EXPECT_EQ(out.bytes[28], 2);
    EXPECT_EQ(out.bytes.size(), le32(out.bytes, 2));
    EXPECT_EQ(level(out, 20, 2), 0);
    EXPECT_EQ(level(out, 20, 61), 3);
  }
}

TEST_F(BmpToBmpTest, DownscalesAndFits) {
  const Output out = convert(splitBmp(160, 128, false, false), false);
  EXPECT_EQ(le32(out.bytes, 18), 40u);
  EXPECT_EQ(le32(out.bytes, 22), 32u);
  EXPECT_EQ(level(out, 5, 1), 0);
  EXPECT_EQ(level(out, 5, 30), 3);
}

TEST_F(BmpToBmpTest, CropsCenterWhileDownscaling) {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(320, 128, topDown, false, true), true);
    EXPECT_EQ(le32(out.bytes, 18), 40u);
    EXPECT_EQ(le32(out.bytes, 22), topDown ? static_cast<uint32_t>(-64) : 64u);
    EXPECT_EQ(level(out, 2, 32), 0);
    EXPECT_EQ(level(out, 37, 32), 3);
  }
}

TEST_F(BmpToBmpTest, OneBitSourceStaysOneBit) {
  const Output out = convert(splitBmp(20, 32, false, true), true);
  EXPECT_EQ(out.bytes[28], 1);
  EXPECT_EQ(le32(out.bytes, 18), 40u);
  EXPECT_EQ(level(out, 3, 2), 0);
  EXPECT_EQ(level(out, 3, 61), 1);
}

TEST_F(BmpToBmpTest, X3CropFillsExactBoundsForSmallAndLargeImages) {
  for (const auto size : {std::pair{120, 80}, std::pair{480, 800}, std::pair{1600, 1200}}) {
    for (const bool topDown : {false, true}) {
      const Output out = convert(splitBmp(size.first, size.second, topDown, false), true, 528, 792);
      EXPECT_EQ(le32(out.bytes, 18), 528u);
      EXPECT_EQ(static_cast<int32_t>(le32(out.bytes, 22)), topDown ? -792 : 792);
      EXPECT_EQ(level(out, 264, 0), 0);
      EXPECT_EQ(level(out, 264, 791), 3);
    }
  }
}

TEST_F(BmpToBmpTest, X3FitUpscalesSmallImageWithItsAspectRatio) {
  const Output out = convert(splitBmp(80, 80, true, false), false, 528, 792);
  EXPECT_EQ(le32(out.bytes, 18), 528u);
  EXPECT_EQ(static_cast<int32_t>(le32(out.bytes, 22)), -528);
  EXPECT_EQ(level(out, 264, 0), 0);
  EXPECT_EQ(level(out, 264, 527), 3);
}

TEST_F(BmpToBmpTest, CropsTallImageFromCenter) {
  // Only the top quarter is black. A centered 1:1 crop must discard it.
  auto bytes = splitBmp(40, 160, true, false);
  const int rowBytes = 40 * 3;
  std::fill(bytes.begin() + 54 + rowBytes * 40, bytes.end(), 0xFF);
  const Output out = convert(bytes, true, 40, 40);
  for (int y = 0; y < 40; ++y) EXPECT_EQ(level(out, 20, y), 3);
}

TEST_F(BmpToBmpTest, PreservesNativeGrayLevelsWhenResizing) {
  std::vector<uint8_t> bytes = {'B', 'M'};
  put32(bytes, 70 + 16 * 16);
  put32(bytes, 0);
  put32(bytes, 70);
  put32(bytes, 40);
  put32(bytes, 64);
  put32(bytes, 16);
  bytes.insert(bytes.end(), {1, 0, 2, 0});
  for (int i = 0; i < 6; ++i) put32(bytes, i == 4 ? 4 : 0);
  for (int level = 0; level < 4; ++level) put32(bytes, level * 0x555555);
  bytes.insert(bytes.end(), 16 * 16, 0x55);  // Native dark gray, index 1.
  const Output out = convert(bytes, true, 128, 32);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 128; ++x) EXPECT_EQ(level(out, x, y), 1);
}

TEST_F(BmpToBmpTest, RejectsInvalidSourceAndTruncatedRows) {
  for (auto bytes : {std::vector<uint8_t>{}, splitBmp(20, 32, false, false)}) {
    bytes.resize(std::min<size_t>(bytes.size(), 56));
    HalFile file(bytes);
    Output out;
    EXPECT_FALSE(BmpToBmpConverter::bmpFileToBmpStream(file, out, 40, 64, true, ImageToneProfile::Legacy));
  }
}

TEST_F(BmpToBmpTest, PropagatesShortWrites) {
  class ShortOutput : public Print {
   public:
    size_t remaining = 90;
    size_t write(uint8_t) override { return remaining ? (--remaining, 1) : 0; }
  } out;
  HalFile file(splitBmp(20, 32, false, false));
  EXPECT_FALSE(BmpToBmpConverter::bmpFileToBmpStream(file, out, 40, 64, true, ImageToneProfile::Legacy));
}

}  // namespace
