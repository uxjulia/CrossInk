#include <Arduino.h>
#include <HalStorage.h>
#include <PoolBudget.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <cstring>

namespace {
void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
  b[o] = v;
  b[o + 1] = v >> 8;
}
void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  put16(b, o, v);
  put16(b, o + 2, v >> 16);
}
template <class T>
void append(std::vector<uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(v));
}
std::vector<uint8_t> fixture() {
  std::vector<uint8_t> b(64, 0);
  memcpy(b.data(), "CPFONT\0\0", 8);
  put16(b, 8, CPFONT_VERSION);
  b[12] = 1;
  put32(b, 36, 2);
  put32(b, 40, 4);
  b[44] = 16;
  put16(b, 45, 12);
  put16(b, 47, uint16_t(-4));
  put32(b, 56, 64);
  append(b, EpdUnicodeInterval{'A', 'C', 0});
  append(b, EpdUnicodeInterval{0xFFFD, 0xFFFD, 3});
  for (uint32_t i = 0; i < 4; i++) {
    EpdGlyph g{};
    g.width = 8;
    g.height = 8;
    g.advanceX = 128;
    g.dataLength = 8;
    g.dataOffset = i * 8;
    append(b, g);
  }
  for (uint8_t i = 0; i < 32; i++) b.push_back(i);
  return b;
}
// Four styles with deliberately distinct advances but usually identical coverage.
std::vector<uint8_t> multiStyleFixture(bool supplementary, bool differentLastStyle = false) {
  std::vector<uint8_t> bytes(32 + 4 * 32, 0);
  memcpy(bytes.data(), "CPFONT\0\0", 8);
  put16(bytes, 8, CPFONT_VERSION);
  bytes[12] = 4;
  for (uint8_t style = 0; style < 4; ++style) {
    const size_t toc = 32 + style * 32;
    bytes[toc] = style;
    put32(bytes, toc + 4, 2);
    put32(bytes, toc + 8, 4);
    bytes[toc + 12] = 16;
    put16(bytes, toc + 13, 12);
    put32(bytes, toc + 24, bytes.size());
    const uint32_t first = supplementary ? 0x10000 : 'A';
    const uint32_t offset = differentLastStyle && style == 3 ? 3 : 0;
    append(bytes, EpdUnicodeInterval{first + offset, first + offset + 2, 0});
    append(bytes, EpdUnicodeInterval{supplementary ? 0x1FFFDu : 0xFFFDu, supplementary ? 0x1FFFDu : 0xFFFDu, 3});
    for (uint32_t i = 0; i < 4; ++i) {
      EpdGlyph glyph{};
      glyph.width = 8;
      glyph.height = 8;
      glyph.advanceX = 128 + style * 16;
      glyph.dataLength = 8;
      glyph.dataOffset = i * 8;
      append(bytes, glyph);
    }
    bytes.resize(bytes.size() + 32, style);
  }
  return bytes;
}
}  // namespace
struct SdFontBitmapStorageTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
    Storage.put("font.cpfont", fixture());
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
};
TEST_F(SdFontBitmapStorageTest, ProductionLoadGrowRetainReleaseAndReload) {
  EpdGlyph decoded;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("A", 1), 0);
  auto* data = font.getEpdFont()->data;
  ASSERT_NE(data->bitmap, nullptr);
  EXPECT_EQ(byteBufferPool(data->bitmap), MemoryPool::Psram);
  EXPECT_EQ(memcmp(data->bitmap, fixture().data() + 64 + 24 + 4 * sizeof(EpdGlyph), 8), 0);
  const auto* first = data->bitmap;
  font.clearCache();
  ASSERT_EQ(font.prewarm("A", 1), 0);
  EXPECT_EQ(font.getEpdFont()->data->bitmap, first);
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  data = font.getEpdFont()->data;
  EXPECT_EQ(byteBufferPool(data->bitmap), MemoryPool::Psram);
  for (uint32_t cp : {uint32_t('A'), uint32_t('B'), uint32_t('C'), 0xFFFDu}) {
    const auto* glyph = font.getEpdFont()->findGlyph(decoded, cp);
    ASSERT_NE(glyph, nullptr);
    const int start = cp == 0xFFFD ? 24 : (cp - 'A') * 8;
    for (int i = 0; i < 8; ++i) EXPECT_EQ(data->bitmap[glyph->dataOffset + i], start + i);
  }
  font.releaseForLowMemory();
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
  ASSERT_EQ(font.prewarm("B", 1), 0);
  EXPECT_NE(font.getEpdFont()->data->bitmap, nullptr);
  ASSERT_TRUE(font.load("font.cpfont"));
  EXPECT_TRUE(fakeheap::live.empty());
}
TEST_F(SdFontBitmapStorageTest, FailedIncrementalGrowthPreservesViewsAndCanRecover) {
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("A", 1), 0);
  const auto* original = font.getEpdFont()->data->bitmap;
  fakeheap::external.fail = 1;
  fakeheap::internal.fail = 1;
  EXPECT_GT(font.prewarm("ABC", 1), 0);
  EXPECT_TRUE(font.lastPrewarmFailed());
  EXPECT_EQ(font.getEpdFont()->data->bitmap, original);
  ASSERT_EQ(fakeheap::live.size(), 1u);
  EXPECT_EQ(original[0], 0);
  EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  EXPECT_NE(font.getEpdFont()->data->bitmap, nullptr);
}
TEST_F(SdFontBitmapStorageTest, C3AndAdmittedInternalFallbackAndMetadataOnly) {
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont"));
    ASSERT_EQ(font.prewarm("A", 1, true), 0);
    EXPECT_TRUE(fakeheap::live.empty());
    EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
    fakeheap::external.fail = 1;
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    EXPECT_EQ(byteBufferPool(font.getEpdFont()->data->bitmap), MemoryPool::Internal);
    font.releaseForLowMemory();
    EXPECT_TRUE(fakeheap::live.empty());
  }
}

TEST_F(SdFontBitmapStorageTest, ExternalBitmapsStillHonorUnderuseAndInternalPressureRelease) {
  EpdGlyph decoded;
  // A dense page followed by disjoint sparse pages must not pin its high-water
  // bitmap forever, even though the payload lives externally.
  std::vector<uint8_t> bytes(64, 0);
  memcpy(bytes.data(), "CPFONT\0\0", 8);
  put16(bytes, 8, CPFONT_VERSION);
  bytes[12] = 1;
  put32(bytes, 36, 2);
  put32(bytes, 40, 601);
  bytes[44] = 16;
  put16(bytes, 45, 12);
  put32(bytes, 56, 64);
  append(bytes, EpdUnicodeInterval{0x100, 0x100 + 599, 0});
  append(bytes, EpdUnicodeInterval{0xFFFD, 0xFFFD, 600});
  for (uint32_t i = 0; i < 601; ++i) {
    EpdGlyph g{};
    g.width = 8;
    g.height = 8;
    g.advanceX = 128;
    g.dataLength = 8;
    g.dataOffset = i * 8;
    append(bytes, g);
  }
  bytes.resize(bytes.size() + 601 * 8, 0xA5);
  Storage.put("large.cpfont", bytes);
  const auto range = [](int first, int count) {
    std::string result;
    for (int i = first; i < first + count; ++i) {
      const uint32_t cp = 0x100 + i;
      result.push_back(char(0xC0 | (cp >> 6)));
      result.push_back(char(0x80 | (cp & 63)));
    }
    return result;
  };
  SdCardFont font;
  ASSERT_TRUE(font.load("large.cpfont"));
  ASSERT_EQ(font.prewarm(range(0, 500).c_str(), 1), 0);
  ASSERT_NE(font.getEpdFont()->data->bitmap, nullptr);
  for (int i = 0; i < 3; ++i) {
    if (i == 1) {
      // The cold replacement left spare bitmap capacity. A failed append must
      // not expose its partly written tail or damage the existing sparse page.
      auto& file = Storage.data("large.cpfont");
      const auto* pixels = font.getEpdFont()->data->bitmap;
      file.readFailAt = 64 + 24 + 601 * sizeof(EpdGlyph) + 525 * 8 + 3;
      EXPECT_GT(font.prewarm(range(525, 25).c_str(), 1), 0);
      EXPECT_EQ(font.getEpdFont()->data->bitmap, pixels);
      EXPECT_EQ(Storage.openHandles("large.cpfont"), 0);
      const auto* resident = font.getEpdFont()->findGlyph(decoded, 0x100 + 500);
      ASSERT_NE(resident, nullptr);
      EXPECT_EQ(pixels[resident->dataOffset], 0xA5);
      file.readFailAt = std::numeric_limits<size_t>::max();
    }
    ASSERT_EQ(font.prewarm(range(500 + i * 25, 25).c_str(), 1), 0);
    font.clearCache();
    EXPECT_EQ(fakeheap::live.empty(), i == 2);
  }
  ASSERT_EQ(font.prewarm(range(0, 20).c_str(), 1), 0);
  fakeheap::internal.free = 39 * 1024;
  font.clearCache();
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
}

TEST_F(SdFontBitmapStorageTest, SharedCoveragePreservesStyleMetricsAndSkipsDuplicateReads) {
  for (bool supplementary : {false, true}) {
    Storage.put("shared.cpfont", multiStyleFixture(supplementary));
    Storage.put("different.cpfont", multiStyleFixture(supplementary, true));
    SdCardFont font;
    ASSERT_TRUE(font.load("shared.cpfont"));
    const size_t sharedReads = Storage.data("shared.cpfont").readBytes;
    const uint32_t first = supplementary ? 0x10000 : 'A';
    for (uint8_t style = 0; style < 4; ++style) {
      ASSERT_NE(font.getEpdFont(style), nullptr);
      EXPECT_TRUE(font.getEpdFont(style)->hasCodepoint(first));
      EXPECT_FALSE(font.getEpdFont(style)->hasCodepoint(first + 3));
      uint16_t advance = 0;
      ASSERT_TRUE(font.readAdvance(first, style, &advance));
      EXPECT_EQ(advance, 128 + style * 16);
    }
    font.releaseForLowMemory();
    // Reload tears down the owner plus three borrowers. The last style now has
    // different coverage, so it must own a separate table even with equal counts.
    ASSERT_TRUE(font.load("different.cpfont"));
    EXPECT_EQ(Storage.data("different.cpfont").readBytes, sharedReads + 2 * sizeof(EpdUnicodeInterval));
    EXPECT_FALSE(font.getEpdFont(3)->hasCodepoint(first));
    EXPECT_TRUE(font.getEpdFont(3)->hasCodepoint(first + 3));
    EXPECT_TRUE(font.getEpdFont(0)->hasCodepoint(first));
    EXPECT_FALSE(font.getEpdFont(0)->hasCodepoint(first + 3));
  }
}

TEST_F(SdFontBitmapStorageTest, FailureAfterSharedStylesCanUnloadAndRetry) {
  auto broken = multiStyleFixture(false);
  const size_t styleBytes = 2 * sizeof(EpdUnicodeInterval) + 4 * sizeof(EpdGlyph) + 32;
  // The first two styles share successfully before the third fails validation.
  put32(broken, 32 + 4 * 32 + 2 * styleBytes, 'Z');
  Storage.put("broken.cpfont", broken);
  Storage.put("shared.cpfont", multiStyleFixture(false));
  SdCardFont font;
  EXPECT_FALSE(font.load("broken.cpfont"));
  EXPECT_EQ(Storage.openHandles("broken.cpfont"), 0);
  ASSERT_TRUE(font.load("shared.cpfont"));
  ASSERT_EQ(font.prewarm("ABC", 0x0F), 0);
  font.clearCache();
  ASSERT_EQ(font.prewarm("A", 0x0F), 0);
}

TEST_F(SdFontBitmapStorageTest, ContentGlyphsSurviveRedrawsAndReleaseTheirPsram) {
  EpdGlyph decoded;
  SdCardFont font;
  const size_t budget = SdCardFont::contentResidentBytes("font.cpfont", 12 * 1024);
  ASSERT_TRUE(font.load("font.cpfont", budget));
  ASSERT_EQ(font.prewarm("ABC", 1, true), 0);
  EXPECT_TRUE(fakeheap::live.empty());
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  ASSERT_EQ(fakeheap::live.size(), 1);
  EXPECT_TRUE(fakeheap::live.begin()->second.external);
  EXPECT_EQ(fakeheap::live.begin()->second.bytes, 128 * 8);
  EXPECT_EQ(fakeheap::internal.attempts, 0);
  const EpdGlyph* last = nullptr;
  for (unsigned pass = 0; pass < 10; ++pass) {
    const size_t reads = Storage.data("font.cpfont").readCalls;
    for (unsigned i = 0; i < 3; ++i) {
      const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'A' + i);
      ASSERT_NE(glyph, nullptr);
      EXPECT_TRUE(font.isOverflowGlyph(glyph));
      const auto* pixels = font.getOverflowBitmap(glyph);
      ASSERT_NE(pixels, nullptr);
      for (unsigned j = 0; j < 8; ++j) EXPECT_EQ(pixels[j], i * 8 + j);
      last = glyph;
    }
    if (pass) EXPECT_EQ(Storage.data("font.cpfont").readCalls, reads);
    // Menus and grayscale passes clear ordinary overflow slots between redraws.
    font.clearCache();
    {
      SdCardFont::MetricsScope measure(&font);
      EXPECT_EQ(font.getEpdFont()->findGlyph(decoded, 'C'), last);
    }
  }
  const size_t attempts = fakeheap::external.attempts;
  font.releaseForLowMemory(true);
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(font.getOverflowBitmap(last), nullptr);
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  EXPECT_EQ(fakeheap::external.attempts, attempts + 1);
  const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'A');
  ASSERT_NE(glyph, nullptr);
  ASSERT_NE(font.getOverflowBitmap(glyph), nullptr);
  EXPECT_EQ(font.getOverflowBitmap(glyph)[0], 0);
  font.clearPersistentCache();
  EXPECT_EQ(font.getOverflowBitmap(glyph), nullptr);
  glyph = font.getEpdFont()->findGlyph(decoded, 'B');
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(font.getOverflowBitmap(glyph)[0], 8);
  ASSERT_TRUE(font.load("font.cpfont", budget));
  EXPECT_TRUE(fakeheap::live.empty());
}

TEST_F(SdFontBitmapStorageTest, PsramContentCacheNeverFallsBackToInternalRamOrRetriesEachFrame) {
  EpdGlyph decoded;
  for (unsigned failure = 1; failure < 4; ++failure) {
    fakeheap::reset(failure != 0);
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont", 12 * 1024));
    if (failure == 1) fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE;
    if (failure == 2) fakeheap::external.largest = 1;
    if (failure == 3) fakeheap::external.fail = 1;
    for (unsigned pass = 0; pass < 3; ++pass) {
      ASSERT_EQ(font.prewarm("ABC", 1), 0);
      const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'B');
      ASSERT_NE(glyph, nullptr);
      ASSERT_NE(font.getOverflowBitmap(glyph), nullptr);
      EXPECT_EQ(font.getOverflowBitmap(glyph)[0], 8);
      font.clearCache();
      EXPECT_TRUE(fakeheap::live.empty());
      EXPECT_EQ(fakeheap::internal.attempts, 0);
      EXPECT_EQ(fakeheap::external.attempts, failure == 3 ? 1 : 0);
    }
  }
}

TEST_F(SdFontBitmapStorageTest, ContentBitmapReadFailureDoesNotPublishPartialPixels) {
  EpdGlyph decoded;
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont", 12 * 1024));
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    // A's eight-byte bitmap begins after the 4-glyph metadata table.
    auto& file = Storage.data("font.cpfont");
    file.readFailAt = 64 + 2 * sizeof(EpdUnicodeInterval) + 4 * sizeof(EpdGlyph) + 3;
    EXPECT_EQ(font.getEpdFont()->findGlyph(decoded, 'A'), nullptr);
    EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
    file.readFailAt = std::numeric_limits<size_t>::max();
    const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'A');
    ASSERT_NE(glyph, nullptr);
    const auto* pixels = font.getOverflowBitmap(glyph);
    ASSERT_NE(pixels, nullptr);
    for (unsigned i = 0; i < 8; ++i) EXPECT_EQ(pixels[i], i);
  }
}

TEST_F(SdFontBitmapStorageTest, ContentBitmapsKeepStylesAndSupplementaryCharactersDistinct) {
  EpdGlyph decoded;
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    Storage.put("styles.cpfont", multiStyleFixture(true));
    SdCardFont font;
    ASSERT_TRUE(font.load("styles.cpfont", 12 * 1024));
    ASSERT_EQ(font.prewarm("", 0x0F), 0);
    for (unsigned pass = 0; pass < 3; ++pass) {
      for (uint8_t style = 0; style < 4; ++style) {
        for (uint32_t cp = 0x10000; cp < 0x10003; ++cp) {
          const auto* glyph = font.getEpdFont(style)->findGlyph(decoded, cp);
          ASSERT_NE(glyph, nullptr);
          EXPECT_EQ(glyph->advanceX, 128 + style * 16);
          const auto* bitmap = font.getOverflowBitmap(glyph);
          ASSERT_NE(bitmap, nullptr);
          for (unsigned i = 0; i < 8; ++i) EXPECT_EQ(bitmap[i], style);
        }
      }
      font.clearCache();
    }
  }
}

TEST_F(SdFontBitmapStorageTest, ContentCacheCollisionsAndOversizedGlyphsPreservePixels) {
  EpdGlyph decoded;
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    constexpr uint32_t count = 512, largeBytes = 640;
    std::vector<uint8_t> bytes(64, 0);
    memcpy(bytes.data(), "CPFONT\0\0", 8);
    put16(bytes, 8, CPFONT_VERSION);
    bytes[12] = 1;
    put32(bytes, 36, 2);
    put32(bytes, 40, count + 1);
    bytes[44] = 16;
    put16(bytes, 45, 12);
    put32(bytes, 56, 64);
    append(bytes, EpdUnicodeInterval{0x100, 0x100 + count - 1, 0});
    append(bytes, EpdUnicodeInterval{0x10000, 0x10000, count});
    for (uint32_t i = 0; i <= count; ++i) {
      EpdGlyph glyph{};
      glyph.width = i == count ? 64 : 8;
      glyph.height = i == count ? 80 : 8;
      glyph.advanceX = 128;
      glyph.dataLength = i == count ? largeBytes : 8;
      glyph.dataOffset = i * 8;
      append(bytes, glyph);
    }
    for (uint32_t i = 0; i < count; ++i) bytes.insert(bytes.end(), 8, uint8_t(i));
    bytes.insert(bytes.end(), largeBytes, 0x5A);
    Storage.put("collisions.cpfont", bytes);
    SdCardFont font;
    ASSERT_TRUE(font.load("collisions.cpfont", 12 * 1024));
    ASSERT_EQ(font.prewarm("", 1), 0);
    ASSERT_EQ(fakeheap::live.size(), 1);
    const size_t expectedBytes =
        psram ? 64 * 1024 : 12 * 1024 - SdCardFont::contentResidentBytes("collisions.cpfont", 12 * 1024);
    EXPECT_EQ(fakeheap::live.begin()->second.bytes, expectedBytes);
    for (unsigned pass = 0; pass < 2; ++pass) {
      // More distinct keys than slots forces eviction without knowing the hash.
      for (uint32_t i = 0; i < count; ++i) {
        const auto* glyph = font.getEpdFont()->findGlyph(decoded, 0x100 + i);
        ASSERT_NE(glyph, nullptr);
        const auto* bitmap = font.getOverflowBitmap(glyph);
        ASSERT_NE(bitmap, nullptr);
        for (unsigned j = 0; j < 8; ++j) EXPECT_EQ(bitmap[j], uint8_t(i));
      }
      const auto* large = font.getEpdFont()->findGlyph(decoded, 0x10000);
      ASSERT_NE(large, nullptr);
      ASSERT_EQ(large->dataLength, largeBytes);
      const auto* bitmap = font.getOverflowBitmap(large);
      ASSERT_NE(bitmap, nullptr);
      for (unsigned j = 0; j < largeBytes; ++j) EXPECT_EQ(bitmap[j], 0x5A);
      font.clearCache();
    }
    EXPECT_EQ(fakeheap::internal.attempts, psram ? 0u : 1u);
    EXPECT_EQ(Storage.openHandles("collisions.cpfont"), 0);
  }
}

TEST_F(SdFontBitmapStorageTest, IncrementalReadsOnlyMissingGlyphsAndPreservesAllPixels) {
  EpdGlyph decoded;
  for (bool psram : {false, true}) {
    fakeheap::reset(psram);
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont"));
    ASSERT_EQ(font.prewarm("C", 1), 0);  // New entries will sort before these residents.
    auto& file = Storage.data("font.cpfont");
    size_t reads = file.readCalls;
    font.resetStats();
    ASSERT_EQ(font.prewarm("AC", 1), 0);
    EXPECT_EQ(file.readCalls - reads, 2u);
    EXPECT_EQ(font.getStats().uniqueGlyphs, 1u);
    EXPECT_EQ(font.getStats().bitmapBytes, 8u);
    reads = file.readCalls;
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    EXPECT_EQ(file.readCalls - reads, 2u);
    const auto* data = font.getEpdFont()->data;
    for (uint32_t cp = 'A'; cp <= 'C'; ++cp) {
      const auto* glyph = font.getEpdFont()->findGlyph(decoded, cp);
      ASSERT_NE(glyph, nullptr);
      for (unsigned b = 0; b < 8; ++b) EXPECT_EQ(data->bitmap[glyph->dataOffset + b], (cp - 'A') * 8 + b);
    }
    reads = file.readCalls;
    font.clearCache();
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    EXPECT_EQ(file.readCalls, reads);
    EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
  }
}

TEST_F(SdFontBitmapStorageTest, IncrementalReadFailurePreservesResidentViewAndRetry) {
  for (bool metadata : {false, true}) {
    for (bool failMetadata : {false, true}) {
      if (metadata && !failMetadata) continue;  // Metadata-only doesn't read pixels.
      SdCardFont font;
      ASSERT_TRUE(font.load("font.cpfont"));
      ASSERT_EQ(font.prewarm("A", 1, metadata), 0);
      const auto* oldData = font.getEpdFont()->data;
      const auto* oldGlyphs = oldData->glyph;
      const auto* oldPixels = oldData->bitmap;
      auto& file = Storage.data("font.cpfont");
      const size_t glyphStart = 64 + 2 * sizeof(EpdUnicodeInterval);
      file.readFailAt = failMetadata ? glyphStart + sizeof(EpdGlyph) + 3 : glyphStart + 4 * sizeof(EpdGlyph) + 8 + 3;
      EXPECT_GT(font.prewarm("AB", 1, metadata), 0);
      EXPECT_TRUE(font.lastPrewarmFailed());
      EXPECT_EQ(oldData->glyph, oldGlyphs);
      EXPECT_EQ(oldData->bitmap, oldPixels);
      EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
      file.readFailAt = std::numeric_limits<size_t>::max();
      ASSERT_EQ(font.prewarm("AB", 1, metadata), 0);
      EXPECT_FALSE(font.lastPrewarmFailed());
    }
  }
}

TEST_F(SdFontBitmapStorageTest, MetadataExtensionAndPromotionNeverConfuseFileAndCacheOffsets) {
  EpdGlyph decoded;
  SdCardFont font;
  ASSERT_TRUE(font.load("font.cpfont"));
  ASSERT_EQ(font.prewarm("C", 1, true), 0);
  auto& file = Storage.data("font.cpfont");
  const size_t reads = file.readCalls;
  ASSERT_EQ(font.prewarm("AC", 1, true), 0);
  EXPECT_EQ(file.readCalls - reads, 1u);
  EXPECT_EQ(font.getEpdFont()->data->bitmap, nullptr);
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = 'A'; cp <= 'C'; ++cp) {
    const auto* glyph = font.getEpdFont()->findGlyph(decoded, cp);
    ASSERT_NE(glyph, nullptr);
    EXPECT_EQ(data->bitmap[glyph->dataOffset], (cp - 'A') * 8);
  }
  // Metadata requests extending a full cache must continue to retain pixels.
  font.releaseForLowMemory();
  ASSERT_EQ(font.prewarm("A", 1), 0);
  const size_t fullReads = file.readCalls;
  ASSERT_EQ(font.prewarm("AB", 1, true), 0);
  EXPECT_EQ(file.readCalls - fullReads, 2u);
  EXPECT_NE(font.getEpdFont()->data->bitmap, nullptr);
  ASSERT_EQ(font.prewarm("AB", 1), 0);
  EXPECT_EQ(file.readCalls - fullReads, 2u);
}

TEST_F(SdFontBitmapStorageTest, IncrementalStylesAndSupplementaryCoverageStayDistinct) {
  EpdGlyph decoded;
  for (bool supplementary : {false, true}) {
    Storage.put("styles.cpfont", multiStyleFixture(supplementary));
    SdCardFont font;
    ASSERT_TRUE(font.load("styles.cpfont"));
    const char* first = supplementary ? "\xf0\x90\x80\x82" : "C";
    const char* next = supplementary ? "\xf0\x90\x80\x80" : "A";
    const uint32_t cp = supplementary ? 0x10000 : 'A';
    font.prewarm(first, 0x0F);  // Supplementary fixture intentionally lacks U+FFFD.
    const size_t reads = Storage.data("styles.cpfont").readCalls;
    font.prewarm(next, 0x0F);
    EXPECT_FALSE(font.lastPrewarmFailed());
    EXPECT_EQ(Storage.data("styles.cpfont").readCalls - reads, 8u);
    for (uint8_t style = 0; style < 4; ++style) {
      for (uint32_t value : {cp, cp + 2}) {
        const auto* glyph = font.getEpdFont(style)->findGlyph(decoded, value);
        ASSERT_NE(glyph, nullptr);
        EXPECT_EQ(glyph->advanceX, 128 + style * 16);
        const auto* data = font.getEpdFont(style)->data;
        for (unsigned b = 0; b < 8; ++b) EXPECT_EQ(data->bitmap[glyph->dataOffset + b], style);
      }
    }
  }
}

TEST_F(SdFontBitmapStorageTest, ZeroBitmapExtensionAndLowMemoryRebuildStayValid) {
  EpdGlyph decoded;
  auto bytes = fixture();
  EpdGlyph space{};
  space.advanceX = 64;
  const size_t offset = 64 + 2 * sizeof(EpdUnicodeInterval) + sizeof(EpdGlyph);
  memcpy(bytes.data() + offset, &space, sizeof(space));
  Storage.put("space.cpfont", bytes);
  SdCardFont font;
  ASSERT_TRUE(font.load("space.cpfont"));
  ASSERT_EQ(font.prewarm("A", 1), 0);
  const auto* original = font.getEpdFont()->data->bitmap;
  const size_t reads = Storage.data("space.cpfont").readCalls;
  ASSERT_EQ(font.prewarm("AB", 1), 0);
  EXPECT_EQ(Storage.data("space.cpfont").readCalls - reads, 1u);
  EXPECT_EQ(font.getEpdFont()->data->bitmap, original);
  const auto* blank = font.getEpdFont()->findGlyph(decoded, 'B');
  ASSERT_NE(blank, nullptr);
  EXPECT_EQ(blank->dataLength, 0);
  EXPECT_EQ(blank->advanceX, 64);
  // Too little headroom for staging: use the existing cold rebuild, then recover.
  fakeheap::internal.free = 40 * 1024 + 1;
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'C');
  ASSERT_NE(glyph, nullptr);
  EXPECT_EQ(font.getEpdFont()->data->bitmap[glyph->dataOffset], 16);
}

TEST_F(SdFontBitmapStorageTest, C3ContentCacheReusesPixelsWithinResidentCeiling) {
  EpdGlyph decoded;
  fakeheap::reset(false);
  const size_t resident = SdCardFont::contentResidentBytes("font.cpfont", 12 * 1024);
  size_t reads[2]{};
  for (unsigned cached = 0; cached < 2; ++cached) {
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont", resident + (cached ? 24 : 0)));
    ASSERT_EQ(font.prewarm("ABC", 1, true), 0);
    EXPECT_TRUE(fakeheap::live.empty());
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    if (cached) {
      ASSERT_EQ(fakeheap::live.size(), 1u);
      EXPECT_FALSE(fakeheap::live.begin()->second.external);
      EXPECT_EQ(fakeheap::live.begin()->second.bytes, 24u);
    } else
      EXPECT_TRUE(fakeheap::live.empty());
    auto& file = Storage.data("font.cpfont");
    const size_t before = file.readCalls;
    const size_t allocations = fakeheap::internal.attempts;
    // 600 characters, repeated over BW and two grayscale passes.
    for (unsigned pass = 0; pass < 3; ++pass) {
      for (unsigned i = 0; i < 600; ++i) {
        const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'A' + i % 3);
        ASSERT_NE(glyph, nullptr);
        const auto* pixels = font.getOverflowBitmap(glyph);
        ASSERT_NE(pixels, nullptr);
        for (unsigned b = 0; b < 8; ++b) ASSERT_EQ(pixels[b], (i % 3) * 8 + b);
      }
      font.clearCache();
      ASSERT_EQ(font.prewarm("ABC", 1), 0);
    }
    reads[cached] = file.readCalls - before;
    EXPECT_EQ(fakeheap::internal.attempts, allocations);
    font.releaseForLowMemory(true);
    EXPECT_TRUE(fakeheap::live.empty());
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'C');
    ASSERT_NE(glyph, nullptr);
    ASSERT_NE(font.getOverflowBitmap(glyph), nullptr);
    EXPECT_EQ(font.getOverflowBitmap(glyph)[0], 16);
    EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
  }
  EXPECT_EQ(reads[0], 3600u);
  EXPECT_EQ(reads[1], 6u);
}

TEST_F(SdFontBitmapStorageTest, C3ContentCachePreservesReservesAndStreamsAfterAllocationFailure) {
  EpdGlyph decoded;
  for (unsigned failure = 0; failure < 3; ++failure) {
    fakeheap::reset(false);
    SdCardFont font;
    ASSERT_TRUE(font.load("font.cpfont", 12 * 1024));
    if (failure == 0) fakeheap::internal.free = MemoryBudget::EPUB_FONT_INTERNAL_RESERVE;
    if (failure == 1) fakeheap::internal.largest = 32 * 1024;
    if (failure == 2) fakeheap::internal.fail = 1;
    for (unsigned pass = 0; pass < 3; ++pass) {
      ASSERT_EQ(font.prewarm("ABC", 1), 0);
      for (uint32_t cp = 'A'; cp <= 'C'; ++cp) {
        const auto* glyph = font.getEpdFont()->findGlyph(decoded, cp);
        ASSERT_NE(glyph, nullptr);
        ASSERT_NE(font.getOverflowBitmap(glyph), nullptr);
        EXPECT_EQ(font.getOverflowBitmap(glyph)[0], (cp - 'A') * 8);
      }
      font.clearCache();
      EXPECT_TRUE(fakeheap::live.empty());
      EXPECT_EQ(fakeheap::internal.attempts, failure == 2 ? 1u : 0u);
    }
    font.releaseForLowMemory();
    fakeheap::internal.free = fakeheap::internal.largest = 100 * 1024;
    ASSERT_EQ(font.prewarm("ABC", 1), 0);
    EXPECT_FALSE(fakeheap::live.empty());
  }
}

TEST_F(SdFontBitmapStorageTest, C3ContentWrapInvalidatesOverlappingPixelsEvenAfterPartialReads) {
  EpdGlyph decoded;
  fakeheap::reset(false);
  auto bytes = fixture();
  // A 12-byte C overlaps both older eight-byte entries when the 16-byte
  // arena wraps. Its final four bytes share D's source pixels intentionally.
  EpdGlyph wider{};
  const size_t glyphOffset = 64 + 2 * sizeof(EpdUnicodeInterval) + 2 * sizeof(EpdGlyph);
  memcpy(&wider, bytes.data() + glyphOffset, sizeof(wider));
  wider.height = wider.dataLength = 12;
  memcpy(bytes.data() + glyphOffset, &wider, sizeof(wider));
  Storage.put("font.cpfont", bytes);
  SdCardFont font;
  const size_t resident = SdCardFont::contentResidentBytes("font.cpfont", 12 * 1024);
  ASSERT_TRUE(font.load("font.cpfont", resident + 16));
  ASSERT_EQ(font.prewarm("ABC", 1), 0);
  const auto* a = font.getEpdFont()->findGlyph(decoded, 'A');
  const auto* b = font.getEpdFont()->findGlyph(decoded, 'B');
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  auto& file = Storage.data("font.cpfont");
  const size_t bitmapStart = 64 + 2 * sizeof(EpdUnicodeInterval) + 4 * sizeof(EpdGlyph);
  file.readFailAt = bitmapStart + 16 + 3;
  EXPECT_EQ(font.getEpdFont()->findGlyph(decoded, 'C'), nullptr);
  EXPECT_EQ(font.getOverflowBitmap(a), nullptr);
  EXPECT_EQ(font.getOverflowBitmap(b), nullptr);
  file.readFailAt = std::numeric_limits<size_t>::max();
  for (unsigned i = 0; i < 100; ++i) {
    const auto* glyph = font.getEpdFont()->findGlyph(decoded, 'A' + i % 3);
    ASSERT_NE(glyph, nullptr);
    const auto* pixels = font.getOverflowBitmap(glyph);
    ASSERT_NE(pixels, nullptr);
    for (unsigned j = 0; j < glyph->dataLength; ++j) EXPECT_EQ(pixels[j], (i % 3) * 8 + j);
  }
  EXPECT_EQ(Storage.openHandles("font.cpfont"), 0);
}
