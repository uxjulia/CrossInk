#include <Bitmap.h>
#include <gtest/gtest.h>

#include <algorithm>
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

}  // namespace

TEST(BitmapResample, PiccoConvertsNativeSourceGraysOnceAtOutputSize) {
  constexpr int width = 128, height = 128, header = 70;
  std::vector<uint8_t> bytes(header + width / 4 * height, 0xaa);  // Native light gray.
  std::fill_n(bytes.begin(), header, 0);
  bytes[0] = 'B';
  bytes[1] = 'M';
  writeLe32(bytes, 2, bytes.size());
  writeLe32(bytes, 10, header);
  writeLe32(bytes, 14, 40);
  writeLe32(bytes, 18, width);
  writeLe32(bytes, 22, height);
  writeLe16(bytes, 26, 1);
  writeLe16(bytes, 28, 2);
  for (int level = 0; level < 4; ++level) std::fill_n(bytes.begin() + 54 + level * 4, 3, level * 85);
  for (auto profile : {ImageToneProfile::Even, ImageToneProfile::PiccoCover}) {
    HalFile file(bytes);
    Bitmap bitmap(file, true, profile);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    const bool source = profile == ImageToneProfile::PiccoCover;
    EXPECT_EQ(bitmap.setDitheredOutputSize(64, 64), source);
    std::vector<uint8_t> scratch(bitmap.getRowBytes()), row(bitmap.getWidth() / 4);
    int whites = 0;
    for (int y = 0; y < bitmap.getHeight(); ++y) {
      ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
      for (int x = 0; x < bitmap.getWidth(); ++x) {
        const int shade = row[x / 4] >> (6 - 2 * (x % 4)) & 3;
        ASSERT_GE(shade, 2);
        whites += shade == 3;
      }
    }
    EXPECT_EQ(whites, 0);
  }
}

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
  EXPECT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::ShortReadRow);

  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  std::vector<uint8_t> secondPass;
  secondPass.reserve(firstPass.size());
  for (int y = 0; y < 792; y++) {
    ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
    secondPass.insert(secondPass.end(), row.begin(), row.end());
  }
  EXPECT_EQ(secondPass, firstPass);
}

TEST(BitmapResample, AveragesThinStripesInsteadOfDroppingThem) {
  for (const bool horizontal : {false, true}) {
    for (const bool topDown : {false, true}) {
      auto bytes = create24BitBmp(4, 4, topDown);
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const uint8_t value = ((horizontal ? y : x) % 2) ? 170 : 0;
          const size_t offset = 54 + y * 12 + x * 3;
          std::fill_n(bytes.begin() + offset, 3, value);
        }
      }
      HalFile file(bytes);
      Bitmap bitmap(file, true, ImageToneProfile::Even);
      ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
      ASSERT_TRUE(bitmap.setDitheredOutputSize(2, 2));
      std::vector<uint8_t> row(1), scratch(bitmap.getRowBytes());
      for (int y = 0; y < 2; ++y) {
        ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
        // Each output pixel averages equal areas of 0 and 170 to native gray 85.
        EXPECT_EQ(row[0], 0x50);
      }
      EXPECT_GE(file.bytesRead(), 4U * 12U);
    }
  }
}

TEST(BitmapResample, FractionalFootprintsShareSourcePixels) {
  for (const bool horizontal : {false, true}) {
    auto bytes = create24BitBmp(3, 3);
    for (int y = 0; y < 3; ++y) {
      for (int x = 0; x < 3; ++x) {
        const uint8_t value = (horizontal ? y : x) == 1 ? 255 : 0;
        std::fill_n(bytes.begin() + 54 + y * 12 + x * 3, 3, value);
      }
    }
    HalFile file(bytes);
    Bitmap bitmap(file, true, ImageToneProfile::Even);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    ASSERT_TRUE(bitmap.setDitheredOutputSize(2, 2));
    std::vector<uint8_t> row(1), scratch(bitmap.getRowBytes());
    for (int pass = 0; pass < 3; ++pass) {
      ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
      for (int y = 0; y < 2; ++y) {
        // Scratch is caller-owned and need not retain the previous source row.
        std::fill(scratch.begin(), scratch.end(), 123);
        ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
        // Each 1.5-pixel footprint contains half of the white middle pixel:
        // (0 * 1 + 255 * 0.5) / 1.5 = 85, on both sides of the boundary.
        EXPECT_EQ(row[0], 0x50);
      }
    }
  }
}

TEST(BitmapResample, RejectsTruncatedSourceRowsDuringAveraging) {
  auto bytes = create24BitBmp(4, 4);
  bytes.pop_back();
  HalFile file(bytes);
  Bitmap bitmap(file, true, ImageToneProfile::Even);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  ASSERT_TRUE(bitmap.setDitheredOutputSize(2, 2));
  std::vector<uint8_t> row(1), scratch(bitmap.getRowBytes());
  ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
  EXPECT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::ShortReadRow);
}

TEST(BitmapResample, HeightOnlyAveragingSupportsHighColorBmpDepths) {
  for (const int bpp : {4, 8, 24, 32}) {
    constexpr int width = 4;
    constexpr int height = 4;
    const int paletteSize = bpp <= 8 ? 1 << bpp : 0;
    const int pixelOffset = 54 + paletteSize * 4;
    const int stride = (width * bpp + 31) / 32 * 4;
    auto bytes = create24BitBmp(width, height);
    bytes.resize(pixelOffset + stride * height, 0);
    writeLe32(bytes, 2, bytes.size());
    writeLe32(bytes, 10, pixelOffset);
    writeLe16(bytes, 28, bpp);
    writeLe32(bytes, 34, stride * height);
    writeLe32(bytes, 46, paletteSize);
    const uint8_t low = 0;
    const uint8_t high = 170;
    for (int i = 0; i < paletteSize; ++i) {
      // An unused non-native entry enables high-color dithering for indexed BMPs.
      std::fill_n(bytes.begin() + 54 + i * 4, 3, i == 0 ? low : i == 1 ? high : 120);
    }
    std::fill(bytes.begin() + pixelOffset, bytes.end(), 0);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int offset = pixelOffset + y * stride;
        if (bpp <= 8) {
          bytes[offset + x * bpp / 8] |= (y % 2) << (8 - bpp - (x * bpp % 8));
        } else {
          std::fill_n(bytes.begin() + offset + x * (bpp / 8), 3, y % 2 ? high : low);
        }
      }
    }
    auto referenceBytes = create24BitBmp(width, 2);
    std::fill(referenceBytes.begin() + 54, referenceBytes.end(), (low + high) / 2);
    HalFile file(bytes), referenceFile(referenceBytes);
    Bitmap bitmap(file, true, ImageToneProfile::Even), reference(referenceFile, true, ImageToneProfile::Even);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    ASSERT_EQ(reference.parseHeaders(), BmpReaderError::Ok);
    ASSERT_TRUE(bitmap.setDitheredOutputSize(width, 2));
    std::vector<uint8_t> row(1), expected(1), scratch(bitmap.getRowBytes()), referenceScratch(reference.getRowBytes());
    for (int y = 0; y < 2; ++y) {
      ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
      ASSERT_EQ(reference.readNextRow(expected.data(), referenceScratch.data()), BmpReaderError::Ok);
      EXPECT_EQ(row, expected) << "bpp=" << bpp;
    }
  }
}

TEST(BitmapResample, MaximumImageCanBeAveragedIntoOnePixelWithoutOverflow) {
  auto bytes = create24BitBmp(2048, 3072);
  std::fill(bytes.begin() + 54, bytes.end(), 255);
  HalFile file(std::move(bytes));
  Bitmap bitmap(file, true, ImageToneProfile::Even);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  ASSERT_TRUE(bitmap.setDitheredOutputSize(1, 1));
  std::vector<uint8_t> scratch(bitmap.getRowBytes());
  uint8_t row = 0;
  ASSERT_EQ(bitmap.readNextRow(&row, scratch.data()), BmpReaderError::Ok);
  EXPECT_EQ(row, 0xC0);
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

TEST(BitmapResample, ResizingKeepsImageQuantizationSeparateFromTextOverlayLevels) {
  auto bytes = create24BitBmp(4, 4);
  std::fill(bytes.begin() + 54, bytes.end(), 85);
  for (bool imageLevels : {false, true}) {
    HalFile file(bytes);
    Bitmap bitmap(file, true, imageToneProfile(imageLevels));
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    ASSERT_TRUE(bitmap.setDitheredOutputSize(2, 2));
    std::vector<uint8_t> row(1), scratch(bitmap.getRowBytes());
    ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
    EXPECT_EQ(row[0] >> 6, imageLevels ? 1 : 2);
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    const auto expected = row;
    ASSERT_EQ(bitmap.readNextRow(row.data(), scratch.data()), BmpReaderError::Ok);
    EXPECT_EQ(row, expected);
  }
}

TEST(BitmapResample, PaletteStartsAfterFullInfoHeader) {
  constexpr int kWidth = 64;
  constexpr int kHeight = 32;
  for (const uint32_t infoHeaderSize : {40u, 108u, 124u}) {
    const size_t paletteOffset = 14 + infoHeaderSize;
    const size_t pixelOffset = paletteOffset + 256 * 4;
    for (const uint8_t pixelIndex : {uint8_t{0}, uint8_t{255}}) {
      std::vector<uint8_t> data(pixelOffset + kWidth * kHeight, pixelIndex);
      std::fill(data.begin(), data.begin() + paletteOffset, 0);
      data[0] = 'B';
      data[1] = 'M';
      writeLe32(data, 2, static_cast<uint32_t>(data.size()));
      writeLe32(data, 10, static_cast<uint32_t>(pixelOffset));
      writeLe32(data, 14, infoHeaderSize);
      writeLe32(data, 18, kWidth);
      writeLe32(data, 22, kHeight);
      writeLe16(data, 26, 1);
      writeLe16(data, 28, 8);
      writeLe32(data, 34, kWidth * kHeight);
      writeLe32(data, 46, 256);
      if (infoHeaderSize > 40) {
        // Extended-header fields must not be mistaken for palette entries.
        writeLe32(data, 54, 0x00FF0000);
        writeLe32(data, 58, 0x0000FF00);
        writeLe32(data, 62, 0x000000FF);
        writeLe32(data, 70, 0x73524742);  // 'sRGB'
      }
      for (int i = 0; i < 256; i++) {
        const size_t entry = paletteOffset + static_cast<size_t>(i) * 4;
        data[entry] = data[entry + 1] = data[entry + 2] = static_cast<uint8_t>(i);
        data[entry + 3] = 0;
      }

      for (const bool imageLevels : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "header=" << infoHeaderSize << " index=" << static_cast<int>(pixelIndex)
                                          << " imageLevels=" << imageLevels);
        HalFile file(data);
        Bitmap bitmap(file, true, imageToneProfile(imageLevels));
        ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
        std::vector<uint8_t> row((kWidth + 3) / 4);
        std::vector<uint8_t> sourceRow(bitmap.getRowBytes());
        const uint8_t expectedByte = pixelIndex == 255 ? 0xFF : 0;
        int unexpectedBytes = 0;
        for (int y = 0; y < kHeight; y++) {
          ASSERT_EQ(bitmap.readNextRow(row.data(), sourceRow.data()), BmpReaderError::Ok);
          for (const uint8_t packed : row) unexpectedBytes += packed != expectedByte;
        }
        EXPECT_EQ(unexpectedBytes, 0);
      }
    }
  }
}

TEST(BitmapResample, RejectsTruncatedExtendedPalette) {
  for (const uint32_t infoHeaderSize : {108u, 124u}) {
    SCOPED_TRACE(infoHeaderSize);
    // The last of the 256 palette entries is missing its red and reserved bytes.
    std::vector<uint8_t> data(14 + infoHeaderSize + 256 * 4 - 2, 0);
    data[0] = 'B';
    data[1] = 'M';
    writeLe32(data, 2, static_cast<uint32_t>(data.size()));
    writeLe32(data, 10, 14 + infoHeaderSize + 256 * 4);
    writeLe32(data, 14, infoHeaderSize);
    writeLe32(data, 18, 1);
    writeLe32(data, 22, 1);
    writeLe16(data, 26, 1);
    writeLe16(data, 28, 8);
    writeLe32(data, 46, 256);

    HalFile file(data);
    Bitmap bitmap(file);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::FileInvalid);
  }
}

TEST(BitmapResample, RejectsPaletteThatOverlapsPixelData) {
  for (const uint32_t infoHeaderSize : {40u, 108u, 124u}) {
    SCOPED_TRACE(infoHeaderSize);
    // The file has enough bytes for 256 reads, but the last entry is actually pixel data.
    const uint32_t pixelOffset = 14 + infoHeaderSize + 255 * 4;
    std::vector<uint8_t> data(pixelOffset + 4, 0);
    std::fill(data.begin() + pixelOffset, data.end(), 255);
    data[0] = 'B';
    data[1] = 'M';
    writeLe32(data, 2, static_cast<uint32_t>(data.size()));
    writeLe32(data, 10, pixelOffset);
    writeLe32(data, 14, infoHeaderSize);
    writeLe32(data, 18, 4);
    writeLe32(data, 22, 1);
    writeLe16(data, 26, 1);
    writeLe16(data, 28, 8);
    writeLe32(data, 46, 256);

    HalFile file(data);
    Bitmap bitmap(file);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::FileInvalid);
  }
}

TEST(BitmapResample, MonochromeExpansionPreservesPalettePixelsAndPadding) {
  for (const auto palette : {std::pair{0, 255}, std::pair{255, 0}, std::pair{85, 170}, std::pair{170, 170}}) {
    for (int width : {1, 2, 3, 4, 5, 7, 8, 9, 255, 480, 1448}) {
      const int stride = (width + 31) / 32 * 4;
      auto bytes = create24BitBmp(width, 2);
      bytes.resize(62 + stride * 2);
      writeLe32(bytes, 2, bytes.size());
      writeLe32(bytes, 10, 62);
      writeLe16(bytes, 28, 1);
      writeLe32(bytes, 34, stride * 2);
      writeLe32(bytes, 46, 2);
      for (int c = 0; c < 3; ++c) {
        bytes[54 + c] = palette.first;
        bytes[58 + c] = palette.second;
      }
      for (int i = 0; i < stride * 2; ++i) bytes[62 + i] = static_cast<uint8_t>(i * 73 + 39);
      HalFile file(bytes);
      Bitmap bitmap(file, true);
      ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
      std::vector<uint8_t> row((width + 3) / 4, 0xFF), source(stride);
      for (int y = 0; y < 2; ++y) {
        ASSERT_EQ(bitmap.readNextRow(row.data(), source.data()), BmpReaderError::Ok);
        std::vector<uint8_t> expected(row.size(), 0);
        for (int x = 0; x < width; ++x) {
          const bool bit = bytes[62 + y * stride + x / 8] & (0x80 >> (x & 7));
          const uint8_t value = adjustPixel(bit ? palette.second : palette.first) >> 6;
          expected[x / 4] |= value << (6 - (x & 3) * 2);
        }
        EXPECT_EQ(row, expected) << "width=" << width;
      }
    }
  }
}
