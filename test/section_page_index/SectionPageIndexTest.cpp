#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "SectionPageIndex.h"

namespace {
struct AllocationState {
  bool failNextChunk = false;
  bool failNextDirectory = false;
  size_t chunkAllocations = 0;
  size_t directoryAllocations = 0;
  size_t largestChunkRequest = 0;
  size_t largestDirectoryRequest = 0;
};

struct TestAllocator {
  AllocationState* state;

  template <typename T>
  std::unique_ptr<T> allocateObject() const {
    state->chunkAllocations++;
    state->largestChunkRequest = std::max(state->largestChunkRequest, sizeof(T));
    if (state->failNextChunk) {
      state->failNextChunk = false;
      return nullptr;
    }
    return std::unique_ptr<T>(new (std::nothrow) T());
  }

  template <typename T>
  std::unique_ptr<T[]> allocateArray(const size_t count) const {
    state->directoryAllocations++;
    state->largestDirectoryRequest = std::max(state->largestDirectoryRequest, sizeof(T) * count);
    if (state->failNextDirectory) {
      state->failNextDirectory = false;
      return nullptr;
    }
    return std::unique_ptr<T[]>(new (std::nothrow) T[count]());
  }
};

using TestIndex = BasicSectionPageIndex<TestAllocator>;

SectionPageIndexEntry entryFor(const size_t index) {
  return {
      static_cast<uint32_t>(1000U + index * 7U),
      static_cast<uint16_t>((index * 3U) & UINT16_MAX),
      static_cast<uint16_t>((index * 5U) & UINT16_MAX),
      static_cast<uint32_t>(index * 11U),
      static_cast<uint32_t>(index * 9U),
  };
}

void expectEntry(const SectionPageIndexEntry& actual, const size_t index) {
  const auto expected = entryFor(index);
  EXPECT_EQ(actual.fileOffset, expected.fileOffset);
  EXPECT_EQ(actual.paragraphIndex, expected.paragraphIndex);
  EXPECT_EQ(actual.listItemIndex, expected.listItemIndex);
  EXPECT_EQ(actual.visibleTextOffset, expected.visibleTextOffset);
  EXPECT_EQ(actual.referenceTextOffset, expected.referenceTextOffset);
}

void append(TestIndex& index, const size_t entryNumber) {
  ASSERT_EQ(index.prepareAppend(), TestIndex::PrepareResult::Ready);
  index.appendPrepared(entryFor(entryNumber));
}

class SectionPageIndexBoundaryTest : public testing::TestWithParam<size_t> {};

TEST_P(SectionPageIndexBoundaryTest, PreservesDistinctEntriesAndRandomAccess) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  const size_t count = GetParam();
  for (size_t i = 0; i < count; ++i) append(index, i);

  EXPECT_EQ(index.size(), count);
  EXPECT_EQ(index.empty(), count == 0);
  for (size_t i = 0; i < count; ++i) expectEntry(index[i], i);
  if (count > 0) {
    expectEntry(index[0], 0);
    expectEntry(index[count / 2], count / 2);
    expectEntry(index[count - 1], count - 1);
  }
}

INSTANTIATE_TEST_SUITE_P(RequiredSizes, SectionPageIndexBoundaryTest,
                         testing::Values(0U, 1U, 63U, 64U, 65U, 511U, 512U, 513U, 1023U, 1024U, 1025U, 65534U, 65535U));

TEST(SectionPageIndex, RejectsThe65536thEntryWithoutChangingPublishedData) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  for (size_t i = 0; i < TestIndex::kMaxEntries; ++i) append(index, i);

  const auto* first = &index[0];
  const auto* last = &index[index.size() - 1];
  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::EntryLimit);
  EXPECT_EQ(index.size(), TestIndex::kMaxEntries);
  EXPECT_EQ(&index[0], first);
  EXPECT_EQ(&index[index.size() - 1], last);
  expectEntry(index[0], 0);
  expectEntry(index[index.size() - 1], TestIndex::kMaxEntries - 1);
}

TEST(SectionPageIndex, RepeatedPreparationDoesNotAllocateOrPublish) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  ASSERT_EQ(index.prepareAppend(), TestIndex::PrepareResult::Ready);
  const size_t chunkAllocations = allocations.chunkAllocations;
  const size_t directoryAllocations = allocations.directoryAllocations;

  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::Ready);
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(allocations.chunkAllocations, chunkAllocations);
  EXPECT_EQ(allocations.directoryAllocations, directoryAllocations);
}

TEST(SectionPageIndex, InitialChunkFailureCanBeRetried) {
  AllocationState allocations{.failNextChunk = true};
  TestIndex index(TestAllocator{&allocations});
  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::AllocationFailure);
  EXPECT_TRUE(index.empty());

  append(index, 0);
  expectEntry(index[0], 0);
}

TEST(SectionPageIndex, InitialDirectoryFailureCanBeRetried) {
  AllocationState allocations{.failNextDirectory = true};
  TestIndex index(TestAllocator{&allocations});
  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::AllocationFailure);
  EXPECT_TRUE(index.empty());

  append(index, 0);
  expectEntry(index[0], 0);
}

TEST(SectionPageIndex, LaterChunkFailureLeavesPublishedEntriesReadable) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  for (size_t i = 0; i < TestIndex::kEntriesPerChunk; ++i) append(index, i);
  const auto* first = &index[0];
  allocations.failNextChunk = true;

  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::AllocationFailure);
  EXPECT_EQ(index.size(), TestIndex::kEntriesPerChunk);
  EXPECT_EQ(&index[0], first);
  expectEntry(index[index.size() - 1], index.size() - 1);

  append(index, TestIndex::kEntriesPerChunk);
  expectEntry(index[TestIndex::kEntriesPerChunk], TestIndex::kEntriesPerChunk);
}

TEST(SectionPageIndex, DirectoryGrowthFailureDoesNotMoveOrCopyPublishedEntries) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  constexpr size_t initialCapacity = TestIndex::kInitialDirectorySlots * TestIndex::kEntriesPerChunk;
  for (size_t i = 0; i < initialCapacity; ++i) append(index, i);
  const auto* first = &index[0];
  const auto* last = &index[index.size() - 1];
  allocations.failNextDirectory = true;

  EXPECT_EQ(index.prepareAppend(), TestIndex::PrepareResult::AllocationFailure);
  EXPECT_EQ(index.size(), initialCapacity);
  EXPECT_EQ(&index[0], first);
  EXPECT_EQ(&index[index.size() - 1], last);
  expectEntry(index[0], 0);
  expectEntry(index[index.size() - 1], initialCapacity - 1);

  append(index, initialCapacity);
  EXPECT_EQ(&index[0], first);
  EXPECT_EQ(&index[initialCapacity - 1], last);
  expectEntry(index[initialCapacity], initialCapacity);
}

TEST(SectionPageIndex, UsesBoundedChunkAndDirectoryAllocations) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  for (size_t i = 0; i < TestIndex::kMaxEntries; ++i) append(index, i);

  EXPECT_EQ(allocations.largestChunkRequest, TestIndex::kEntriesPerChunk * sizeof(SectionPageIndexEntry));
  EXPECT_EQ(allocations.largestDirectoryRequest, TestIndex::kMaxDirectorySlots * sizeof(std::unique_ptr<int>));
  EXPECT_EQ(allocations.chunkAllocations, TestIndex::kMaxDirectorySlots);
}

TEST(SectionPageIndex, ClearReleasesStorageAndResetsTheIndex) {
  AllocationState allocations;
  TestIndex index(TestAllocator{&allocations});
  for (size_t i = 0; i < 65; ++i) append(index, i);

  index.clear();
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.size(), 0U);
  append(index, 0);
  expectEntry(index[0], 0);
}
}  // namespace
