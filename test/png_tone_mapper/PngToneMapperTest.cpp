#include <ImageToneMapper.h>
#include <gtest/gtest.h>

#include <new>
#include <vector>

#include "lib/Epub/Epub/converters/PngToneMapperPolicy.h"
#include "lib/GfxRenderer/BitmapHelpers.h"

namespace {
int failAllocation = -1;
struct Output {
  int rows = 0;
  std::vector<uint8_t> levels;
  static void sink(void* context, int y, const uint8_t* row, int width) {
    auto& out = *static_cast<Output*>(context);
    EXPECT_EQ(y, out.rows++);
    out.levels.insert(out.levels.end(), row, row + width);
  }
};
}  // namespace
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (failAllocation >= 0 && failAllocation-- == 0) return nullptr;
  return ::operator new[](size);
}

TEST(PngToneMapper, AdmissionPreservesHeadroomAndLargestBlock) {
  using PngToneMapperPolicy::admits;
  const uint32_t total = 2 * 800 + 17 * 400 + 16;
  EXPECT_TRUE(admits(800, 600, 400, 300, total + 16384 + 512, 4 * 402 + 512));
  EXPECT_FALSE(admits(800, 600, 400, 300, total + 16384 + 511, 100000));
  EXPECT_FALSE(admits(800, 600, 400, 300, 100000, 4 * 402 + 511));
  EXPECT_FALSE(admits(800, 600, 800, 600, 100000, 100000));
  EXPECT_FALSE(admits(800, 600, 900, 300, 100000, 100000));
  EXPECT_FALSE(admits(800, 600, 400, 0, 100000, 100000));
  EXPECT_FALSE(admits(2048, 3072, 1, 1, 100000, 100000));
  EXPECT_TRUE(admits(255, 257, 1, 1, 100000, 100000));  // 65535 samples exactly
  EXPECT_FALSE(admits(256, 256, 1, 1, 100000, 100000));
  EXPECT_FALSE(admits(INT32_MAX, INT32_MAX, 1, 1, UINT32_MAX, UINT32_MAX));
}

TEST(PngToneMapper, EveryAllocationFailureAllowsFreshFallbackOrRetry) {
  for (int index = 0; index < 8; ++index) {
    freeink::ImageToneMapper mapper;
    Output output;
    failAllocation = index;
    EXPECT_FALSE(mapper.begin(16, 16, 8, 8, 4, Output::sink, &output));
    failAllocation = -1;
    mapper.finish();
    EXPECT_EQ(output.rows, 0);
    EXPECT_TRUE(mapper.begin(16, 16, 8, 8, 4, Output::sink, &output));
  }
}

TEST(PngToneMapper, ThinLinesAndCheckerboardDoNotAliasToWhiteOrBlack) {
  for (bool checkerboard : {false, true}) {
    freeink::ImageToneMapper mapper;
    Output output;
    output.levels.reserve(16);
    ASSERT_TRUE(mapper.begin(8, 8, 4, 4, 4, Output::sink, &output));
    uint8_t row[8];
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) row[x] = ((x + (checkerboard ? y : 0)) & 1) ? 255 : 0;
      mapper.addSourceRow(row);
    }
    mapper.finish();
    mapper.finish();
    EXPECT_EQ(output.rows, 4);
    ASSERT_EQ(output.levels.size(), 16);
    for (auto level : output.levels) EXPECT_TRUE(level == 1 || level == 2);
  }
}

TEST(PngToneMapper, NonIntegerBinsAndLastRowAreFullyAveraged) {
  freeink::ImageToneMapper mapper;
  Output output;
  output.levels.reserve(4);
  ASSERT_TRUE(mapper.begin(5, 3, 2, 2, 4, Output::sink, &output));
  // First destination bin sees 0+0+255 => 85, second sees 255+85 =>170.
  const uint8_t top[] = {0, 0, 255, 255, 85};
  mapper.addSourceRow(top);
  mapper.addSourceRow(top);
  const uint8_t bottom[] = {255, 255, 255, 0, 0};
  mapper.addSourceRow(bottom);
  mapper.finish();
  EXPECT_EQ(output.levels, (std::vector<uint8_t>{1, 2, 3, 0}));
}

TEST(PngToneMapper, AlphaIsCompositedBeforeAveragingAndPlanesPreserveLevels) {
  EXPECT_EQ(PngToneMapperPolicy::blendWhite(0, 0), 255);
  EXPECT_EQ(PngToneMapperPolicy::blendWhite(0, 255), 0);
  EXPECT_EQ(PngToneMapperPolicy::blendWhite(0, 170), 85);
  freeink::ImageToneMapper mapper;
  Output output;
  output.levels.reserve(4);
  ASSERT_TRUE(mapper.begin(8, 2, 4, 1, 4, Output::sink, &output));
  uint8_t row[8];
  for (int x = 0; x < 8; ++x) row[x] = PngToneMapperPolicy::blendWhite(0, 255 - (x / 2) * 85);
  mapper.addSourceRow(row);
  mapper.addSourceRow(row);
  mapper.finish();
  ASSERT_EQ(output.levels, (std::vector<uint8_t>{0, 1, 2, 3}));
  // PXC packs most significant sample first. Absolute planes reconstruct the
  // same levels, not an ordered-dither reinterpretation of continuous tone.
  uint8_t packed = 0, msb = 0, lsb = 0;
  for (int x = 0; x < 4; ++x) {
    packed |= output.levels[x] << (6 - 2 * x);
    const auto hi = grayPlanePixel(output.levels[x], true, true);
    const auto lo = grayPlanePixel(output.levels[x], false, true);
    ASSERT_TRUE(hi.write && lo.write);
    if (!hi.black) msb |= 1 << (3 - x);
    if (!lo.black) lsb |= 1 << (3 - x);
  }
  EXPECT_EQ(packed, 0x1b);
  EXPECT_EQ(msb, 0x03);
  EXPECT_EQ(lsb, 0x05);
}

TEST(PngToneMapper, ReplayCannotChangeQuantizersWhenHeapAdmissionChanges) {
  for (bool dithering : {false, true})
    for (bool bw : {false, true})
      for (bool caching : {false, true})
        EXPECT_EQ(PngToneMapperPolicy::mayMap(dithering, bw, caching), dithering && bw && caching);
  // A standalone viewer has no PXC; neither its BW base nor either selector
  // can opt into optional mapping, regardless of subsequent allocation state.
  for (bool bw : {false, true}) EXPECT_FALSE(PngToneMapperPolicy::mayMap(true, bw, false));
}
