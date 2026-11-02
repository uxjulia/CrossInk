#include <JPEGDEC.h>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
struct DecodedImage {
  int width;
  int height;
  std::vector<uint8_t> pixels;
  std::vector<int> visits;
};

int collectPixels(JPEGDRAW* draw) {
  auto& image = *static_cast<DecodedImage*>(draw->pUser);
  const auto* pixels = reinterpret_cast<const uint8_t*>(draw->pPixels);
  for (int y = 0; y < draw->iHeight; ++y) {
    for (int x = 0; x < draw->iWidthUsed; ++x) {
      const int dstX = draw->x + x;
      const int dstY = draw->y + y;
      if (dstX < 0 || dstX >= image.width || dstY < 0 || dstY >= image.height) {
        ADD_FAILURE() << "JPEG callback outside output bounds";
        return 0;
      }
      const size_t index = static_cast<size_t>(dstY) * image.width + dstX;
      image.pixels[index] = pixels[y * draw->iWidth + x];
      ++image.visits[index];
    }
  }
  return 1;
}

class JpegDecoderTest : public ::testing::TestWithParam<const char*> {};

TEST_P(JpegDecoderTest, KeepsStripeColumnsAlignedAcrossRows) {
  std::ifstream file(std::string(JPEG_FIXTURE_DIR) + "/" + GetParam() + ".jpg", std::ios::binary);
  ASSERT_TRUE(file.is_open());
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  file.close();
  JPEGDEC decoder;
  ASSERT_EQ(decoder.openRAM(bytes.data(), static_cast<int>(bytes.size()), collectPixels), 1);
  const int width = (decoder.getWidth() + 7) / 8;
  const int height = (decoder.getHeight() + 7) / 8;
  DecodedImage image{width, height, std::vector<uint8_t>(width * height), std::vector<int>(width * height)};
  decoder.setPixelType(EIGHT_BIT_GRAYSCALE);
  decoder.setUserPointer(&image);
  ASSERT_EQ(decoder.decode(0, 0, JPEG_SCALE_EIGHTH), 1);
  decoder.close();

  constexpr std::array<int, 3> levels{32, 128, 224};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t index = static_cast<size_t>(y) * width + x;
      ASSERT_EQ(image.visits[index], 1) << "at " << x << "," << y;
      ASSERT_NEAR(image.pixels[index], levels[x % levels.size()], 2) << "at " << x << "," << y;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ScanLayouts, JpegDecoderTest,
                         ::testing::Values("luma-422", "luma-420", "luma-440", "luma-444", "luma-422-partial",
                                           "luma-422-wide", "interleaved-422", "interleaved-420", "baseline-422"));
}  // namespace
