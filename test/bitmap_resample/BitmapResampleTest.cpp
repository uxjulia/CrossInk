#include <Bitmap.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {
void writeLe16(std::vector<uint8_t>& data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(std::vector<uint8_t>& data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> create24BitBmp(const int width, const int height, const bool topDown = false) {
  const int rowBytes = (width * 24 + 31) / 32 * 4;
  constexpr size_t kPixelOffset = 54;
  std::vector<uint8_t> data(kPixelOffset + static_cast<size_t>(rowBytes) * height, 0);
  data[0] = 'B';
  data[1] = 'M';
  writeLe32(data, 2, static_cast<uint32_t>(data.size()));
  writeLe32(data, 10, kPixelOffset);
  writeLe32(data, 14, 40);
  writeLe32(data, 18, width);
  writeLe32(data, 22, static_cast<uint32_t>(topDown ? -height : height));
  writeLe16(data, 26, 1);
  writeLe16(data, 28, 24);
  writeLe32(data, 34, static_cast<uint32_t>(rowBytes * height));

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const uint8_t value = static_cast<uint8_t>((x * 13 + y * 7) & 0xFF);
      const int fileRow = topDown ? y : height - 1 - y;
      const size_t offset = kPixelOffset + static_cast<size_t>(fileRow) * rowBytes + x * 3;
      data[offset] = value;
      data[offset + 1] = value;
      data[offset + 2] = value;
    }
  }
  return data;
}

uint32_t fingerprint(const std::vector<uint8_t>& data) {
  uint32_t hash = 2166136261U;
  for (const uint8_t value : data) {
    hash = (hash ^ value) * 16777619U;
  }
  return hash;
}
}  // namespace

TEST(BitmapResample, DownsamplesBeforeDitheringAndRewindsDeterministically) {
  HalFile file(create24BitBmp(480, 800));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  ASSERT_TRUE(bitmap.setDitheredOutputSize(475, 792));
  EXPECT_EQ(bitmap.getWidth(), 475);
  EXPECT_EQ(bitmap.getHeight(), 792);

  std::vector<uint8_t> firstPass;
  std::vector<uint8_t> row((475 + 3) / 4);
  std::vector<uint8_t> sourceRow(bitmap.getRowBytes());
  firstPass.reserve(row.size() * 792);
  for (int y = 0; y < 792; y++) {
    ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
    firstPass.insert(firstPass.end(), row.begin(), row.end());
  }
  EXPECT_EQ(fingerprint(firstPass), 819650312U);

  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  std::vector<uint8_t> secondPass;
  secondPass.reserve(firstPass.size());
  for (int y = 0; y < 792; y++) {
    ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
    secondPass.insert(secondPass.end(), row.begin(), row.end());
  }
  EXPECT_EQ(secondPass, firstPass);
}

TEST(BitmapResample, ReadsPhysicalRowsBeforeRendererOrientation) {
  HalFile bottomUpFile(create24BitBmp(480, 800));
  HalFile topDownFile(create24BitBmp(480, 800, true));
  Bitmap bottomUp(bottomUpFile, true);
  Bitmap topDown(topDownFile, true);
  ASSERT_EQ(bottomUp.parseHeaders(), BmpReaderError::Ok);
  ASSERT_EQ(topDown.parseHeaders(), BmpReaderError::Ok);
  EXPECT_FALSE(bottomUp.isTopDown());
  EXPECT_TRUE(topDown.isTopDown());
  ASSERT_TRUE(bottomUp.setDitheredOutputSize(475, 792));
  ASSERT_TRUE(topDown.setDitheredOutputSize(475, 792));

  std::vector<uint8_t> bottomUpRow((475 + 3) / 4);
  std::vector<uint8_t> topDownRow((475 + 3) / 4);
  std::vector<uint8_t> bottomUpSourceRow(bottomUp.getRowBytes());
  std::vector<uint8_t> topDownSourceRow(topDown.getRowBytes());
  ASSERT_EQ(bottomUp.readNextRow(bottomUpRow.data(), bottomUpSourceRow.data()), BmpReaderError::Ok);
  ASSERT_EQ(topDown.readNextRow(topDownRow.data(), topDownSourceRow.data()), BmpReaderError::Ok);

  // The same visual gradient is encoded bottom-to-top or top-to-bottom. The
  // decoder must preserve each file's physical order; GfxRenderer uses
  // isTopDown() to place these rows on their matching screen edge.
  EXPECT_EQ(bottomUpRow.front() >> 6, 3U);
  EXPECT_EQ(topDownRow.front() >> 6, 0U);
}
