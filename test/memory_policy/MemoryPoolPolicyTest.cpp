#include <Epub/PageDeserializationBudget.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PoolBudget.h>
#include <gtest/gtest.h>

struct MemoryPoolPolicyTest : testing::Test {
  void SetUp() override { fakeheap::reset(); }
  void TearDown() override { EXPECT_TRUE(fakeheap::live.empty()); }
};
TEST_F(MemoryPoolPolicyTest, OverflowAndFragmentationCannotAdmitOperation) {
  using namespace MemoryBudget;
  EXPECT_FALSE(admits({SIZE_MAX, SIZE_MAX, SIZE_MAX}, {SIZE_MAX, 1, 1}));
  EXPECT_TRUE(admits({SIZE_MAX, SIZE_MAX, SIZE_MAX}, {SIZE_MAX, SIZE_MAX, 0}));
  EXPECT_FALSE(admits({1000, 1000, 10}, {100, 100, 0}));
  EXPECT_FALSE(admitsOperation({20, 20, 20}, {1000, 1000, 1000}, {21, 1, 0}, {0, 0, 0}));
  EXPECT_FALSE(admitsOperation({1000, 1000, 1000}, {20, 20, 20}, {0, 0, 0}, {21, 1, 0}));
}
TEST_F(MemoryPoolPolicyTest, ExplicitPoolsAndCorrectDeallocation) {
  fakeheap::defaultExternal = true;
  auto internal = makeInternalByteBufferNoThrow(100);
  auto external = makePsramByteBufferNoThrow(100);
  auto defaults = makeHeapByteBufferNoThrow(100);
  EXPECT_EQ(byteBufferPool(internal.get()), MemoryPool::Internal);
  EXPECT_EQ(byteBufferPool(external.get()), MemoryPool::Psram);
  EXPECT_EQ(byteBufferPool(defaults.get()), MemoryPool::Psram);
  EXPECT_EQ(fakeheap::live.size(), 3u);
}
TEST_F(MemoryPoolPolicyTest, AdmissionDoesNotGuaranteeAllocation) {
  ASSERT_TRUE(MemoryBudget::canAllocatePsram(100));
  fakeheap::external.fail = 1;
  EXPECT_FALSE(makePsramByteBufferNoThrow(100));
  fakeheap::reset(false);
  EXPECT_FALSE(psramHeapAvailable());
  EXPECT_FALSE(MemoryBudget::canAllocatePsram(1));
  EXPECT_FALSE(makePsramByteBufferNoThrow(1));
  EXPECT_FALSE(makeInternalByteBufferNoThrow(0));
}

TEST_F(MemoryPoolPolicyTest, PageVectorAdmissionChecksContiguousBytesAndSmallHeadroom) {
  using PageDeserializationBudget::MIN_FREE_AFTER_RESERVE;
  constexpr size_t pageElementPointers = 1024 * sizeof(void*);
  EXPECT_TRUE(PageDeserializationBudget::canReserveVector(1024, sizeof(void*), pageElementPointers + 1024,
                                                          pageElementPointers));
  EXPECT_FALSE(PageDeserializationBudget::canReserveVector(1024, sizeof(void*), pageElementPointers + 1024,
                                                           pageElementPointers - 1));
  EXPECT_FALSE(PageDeserializationBudget::canReserveVector(
      1024, sizeof(void*), pageElementPointers + MIN_FREE_AFTER_RESERVE - 1, pageElementPointers));
  EXPECT_TRUE(PageDeserializationBudget::canReserveVector(0, sizeof(void*), 0, 0));
  EXPECT_FALSE(PageDeserializationBudget::canReserveVector(SIZE_MAX, 2, SIZE_MAX, SIZE_MAX));
}

TEST_F(MemoryPoolPolicyTest, PageDeserializationRejectsTruncatedDenseCountsBeforeReserving) {
  EXPECT_TRUE(PageDeserializationBudget::hasMinimumSerializedBytes(1027, 1024, 1, 3));
  EXPECT_FALSE(PageDeserializationBudget::hasMinimumSerializedBytes(1026, 1024, 1, 3));
  EXPECT_TRUE(PageDeserializationBudget::hasMinimumSerializedBytes(1024 * 8 + 3, 1024, 8, 3));
  EXPECT_FALSE(PageDeserializationBudget::hasMinimumSerializedBytes(1024 * 8 + 2, 1024, 8, 3));
  EXPECT_TRUE(PageDeserializationBudget::hasMinimumSerializedBytes(64 * 4, 64, 4));
  EXPECT_FALSE(PageDeserializationBudget::hasMinimumSerializedBytes(64 * 4 - 1, 64, 4));
  EXPECT_FALSE(PageDeserializationBudget::hasMinimumSerializedBytes(SIZE_MAX, SIZE_MAX, 2));
}

TEST_F(MemoryPoolPolicyTest, PageDeserializationReusesHeapShapeForSmallVectorsAndDebitsTheirBytes) {
  PageDeserializationBudget::HeapBudget budget;
  budget.refresh(512, 256);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(budget.needsRefresh(100, sizeof(uint8_t)));
    ASSERT_TRUE(budget.canReserve(100, sizeof(uint8_t)));
    budget.recordReserve(100, sizeof(uint8_t));
  }
  EXPECT_FALSE(budget.canReserve(100, sizeof(uint8_t)));
  EXPECT_TRUE(budget.needsRefresh(1, 1024));
}

TEST_F(MemoryPoolPolicyTest, JpegDecoderPrefersPsramWhileRetainingInternalReserve) {
  constexpr size_t decoderBytes = MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM;
  fakeheap::internal.largest = 1;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + decoderBytes;
  fakeheap::external.largest = decoderBytes;

  EXPECT_EQ(MemoryBudget::jpegDecoderPool(decoderBytes), MemoryPool::Psram);
}

TEST_F(MemoryPoolPolicyTest, JpegDecoderRejectsPsramThatWouldConsumeInternalReserve) {
  constexpr size_t decoderBytes = MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM - 1;
  fakeheap::internal.largest = 1;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + decoderBytes;
  fakeheap::external.largest = decoderBytes;

  EXPECT_EQ(MemoryBudget::jpegDecoderPool(decoderBytes), MemoryPool::None);
}

TEST_F(MemoryPoolPolicyTest, JpegDecoderUsesUnchangedInternalThresholdWhenPsramIsUnavailable) {
  constexpr size_t decoderBytes = MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::reset(false);
  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_JPEG_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC;

  EXPECT_EQ(MemoryBudget::jpegDecoderPool(decoderBytes), MemoryPool::Internal);
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC - 1;
  EXPECT_EQ(MemoryBudget::jpegDecoderPool(decoderBytes), MemoryPool::None);
}

TEST_F(MemoryPoolPolicyTest, JpegDecoderRejectsFragmentedPsramAndSafelyFallsBackToInternal) {
  constexpr size_t decoderBytes = MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_JPEG_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + decoderBytes;
  fakeheap::external.largest = decoderBytes - 1;

  EXPECT_EQ(MemoryBudget::jpegDecoderPool(decoderBytes), MemoryPool::Internal);
}

TEST_F(MemoryPoolPolicyTest, InlineImageAdmissionUsesPsramOnlyForJpeg) {
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM;
  fakeheap::internal.largest = 1;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + MemoryBudget::JPEG_DECODER_APPROX_BYTES;
  fakeheap::external.largest = MemoryBudget::JPEG_DECODER_APPROX_BYTES;

  EXPECT_TRUE(MemoryBudget::hasHeapForEpubInlineImage("TEST", "image.jpg"));
  EXPECT_TRUE(MemoryBudget::hasHeapForEpubInlineImage("TEST", "image.JPEG"));
  EXPECT_FALSE(MemoryBudget::hasHeapForEpubInlineImage("TEST", "image.png"));

  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_IMAGE_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_IMAGE_MIN_MAX_ALLOC;
  EXPECT_TRUE(MemoryBudget::hasHeapForEpubInlineImage("TEST", "image.png"));

  fakeheap::internal.free = MemoryBudget::EPUB_OPTIMIZER_PXC_MIN_FREE - 1;
  fakeheap::internal.largest = MemoryBudget::EPUB_OPTIMIZER_PXC_MIN_MAX_ALLOC;
  EXPECT_FALSE(MemoryBudget::hasHeapForOptimizerPxcImage("TEST", "image.pxc"));
  fakeheap::internal.free = MemoryBudget::EPUB_OPTIMIZER_PXC_MIN_FREE;
  EXPECT_TRUE(MemoryBudget::hasHeapForOptimizerPxcImage("TEST", "image.pxc"));
}

namespace {
struct DecoderOwnerProbe {
  static inline int destructed = 0;
  ~DecoderOwnerProbe() { ++destructed; }
};
}  // namespace

TEST_F(MemoryPoolPolicyTest, CapabilityObjectOwnerDestroysAndFreesItsPsramObject) {
  DecoderOwnerProbe::destructed = 0;
  {
    HeapObject<DecoderOwnerProbe> decoder;
    ASSERT_TRUE(decoder.init(MemoryPool::Psram));
    EXPECT_EQ(decoder.pool(), MemoryPool::Psram);
    EXPECT_EQ(fakeheap::live.size(), 1u);
  }
  EXPECT_EQ(DecoderOwnerProbe::destructed, 1);
}

TEST_F(MemoryPoolPolicyTest, FailedPsramObjectAllocationLeavesSafeInternalFallbackAvailable) {
  DecoderOwnerProbe::destructed = 0;
  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_JPEG_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC;
  fakeheap::external.fail = 1;
  {
    HeapObject<DecoderOwnerProbe> decoder;
    EXPECT_FALSE(decoder.init(MemoryPool::Psram));
    EXPECT_TRUE(MemoryBudget::canUseInternalHeapForJpegDecoder(byteHeapSnapshot(MemoryPool::Internal)));
    ASSERT_TRUE(decoder.init(MemoryPool::Internal));
    EXPECT_EQ(decoder.pool(), MemoryPool::Internal);
  }
  EXPECT_EQ(DecoderOwnerProbe::destructed, 1);
}

#include <Arena.h>
#include <ArenaVector.h>
TEST_F(MemoryPoolPolicyTest, ArenaAlignmentGrowthCheckpointAndReuse) {
  Arena a(ArenaBacking::PsramOnly);
  ASSERT_TRUE(a.init(64));
  ASSERT_NE(a.alloc(1, 1), nullptr);
  auto* aligned = a.alloc(sizeof(std::max_align_t));
  ASSERT_NE(aligned, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % alignof(std::max_align_t), 0u);
  auto cp = a.save();
  ASSERT_NE(a.alloc(100), nullptr);
  ASSERT_NE(a.alloc(200), nullptr);
  EXPECT_EQ(fakeheap::live.size(), 3u);
  a.restore(cp);
  EXPECT_EQ(fakeheap::live.size(), 1u);
  EXPECT_EQ(a.current->offset, cp.offset);
  a.clear();
  EXPECT_EQ(a.used(), 0u);
  ASSERT_NE(a.alloc(64), nullptr);
  EXPECT_EQ(a.capacityInPool(MemoryPool::Internal), 0u);
  EXPECT_GT(a.capacityInPool(MemoryPool::Psram), 64u);
}
TEST_F(MemoryPoolPolicyTest, ArenaFailuresAndMixedFallbackDoNotLeak) {
  Arena a(ArenaBacking::PsramPreferred);
  fakeheap::external.fail = 1;
  ASSERT_TRUE(a.init(64));
  EXPECT_EQ(a.head->pool, MemoryPool::Internal);
  ASSERT_NE(a.alloc(128), nullptr);
  EXPECT_EQ(a.current->pool, MemoryPool::Psram);
  auto cp = a.save();
  fakeheap::external.fail = 1;
  fakeheap::internal.largest = 1;
  EXPECT_EQ(a.alloc(1000), nullptr);
  EXPECT_EQ(a.current, cp.slab);
  EXPECT_EQ(fakeheap::live.size(), 2u);
  a.release();
  Arena optional(ArenaBacking::PsramOnly);
  fakeheap::external.fail = 1;
  EXPECT_FALSE(optional.init(100));
  EXPECT_EQ(optional.alloc(10), nullptr);
  EXPECT_TRUE(fakeheap::live.empty());
}
TEST_F(MemoryPoolPolicyTest, ArenaRejectsOverflowAndUnsupportedAlignment) {
  Arena a;
  EXPECT_FALSE(a.init(SIZE_MAX));
  ASSERT_TRUE(a.init(64));
  EXPECT_EQ(a.alloc(SIZE_MAX), nullptr);
  EXPECT_EQ(a.alloc(10, 0), nullptr);
  EXPECT_EQ(a.alloc(10, 3), nullptr);
  EXPECT_EQ(a.alloc(10, alignof(std::max_align_t) * 2), nullptr);
  EXPECT_EQ(arenaNewArray<uint64_t>(a, SIZE_MAX), nullptr);
  ArenaVector<uint64_t> v(a);
  EXPECT_FALSE(v.reserve(SIZE_MAX));
  EXPECT_EQ(fakeheap::live.size(), 1u);
  ASSERT_TRUE(a.init(32));  // reinit releases the old slab
  EXPECT_EQ(fakeheap::live.size(), 1u);
}
