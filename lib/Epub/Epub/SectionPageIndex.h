#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

struct SectionPageIndexEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t visibleTextOffset;
  // Recorded even when metadata is skipped (for example sleep-page rebuilds),
  // so the same cache remains usable when reopened with stable-page metadata.
  uint32_t referenceTextOffset = 0;
};

static_assert(sizeof(SectionPageIndexEntry) == 16, "Section page index entry size changed");

struct SectionPageIndexAllocator {
  template <typename T>
  std::unique_ptr<T> allocateObject() const {
    return makeUniqueNoThrow<T>();
  }

  template <typename T>
  std::unique_ptr<T[]> allocateArray(const size_t count) const {
    return makeUniqueNoThrow<T[]>(count);
  }
};

template <typename Allocator = SectionPageIndexAllocator>
class BasicSectionPageIndex {
 public:
  using Entry = SectionPageIndexEntry;

  static constexpr size_t kEntriesPerChunk = 64;
  static constexpr size_t kInitialDirectorySlots = 8;
  static constexpr size_t kMaxDirectorySlots = 1024;
  static constexpr size_t kMaxEntries = UINT16_MAX;

  enum class PrepareResult : uint8_t {
    Ready,
    EntryLimit,
    AllocationFailure,
  };

  explicit BasicSectionPageIndex(Allocator allocator = {}) : allocator_(std::move(allocator)) {}

  BasicSectionPageIndex(const BasicSectionPageIndex&) = delete;
  BasicSectionPageIndex& operator=(const BasicSectionPageIndex&) = delete;
  BasicSectionPageIndex(BasicSectionPageIndex&&) = default;
  BasicSectionPageIndex& operator=(BasicSectionPageIndex&&) = default;

  PrepareResult prepareAppend() {
    if (size_ >= kMaxEntries) return PrepareResult::EntryLimit;

    const size_t chunkIndex = size_ / kEntriesPerChunk;
    if (chunkIndex < directorySlots_ && directory_[chunkIndex]) return PrepareResult::Ready;

    // A 1 KiB chunk is kept off the small task stack and allocated only when
    // its first page arrives. It remains owned until clear() or index destruction.
    auto chunk = allocator_.template allocateObject<Chunk>();
    if (!chunk) return PrepareResult::AllocationFailure;

    if (chunkIndex >= directorySlots_) {
      size_t nextSlots = directorySlots_ == 0 ? kInitialDirectorySlots : directorySlots_ * 2;
      if (nextSlots > kMaxDirectorySlots) nextSlots = kMaxDirectorySlots;
      if (chunkIndex >= nextSlots) return PrepareResult::EntryLimit;

      // The directory is dynamic ownership metadata, so stack/static storage is
      // unsuitable. It starts at 8 pointers (32 bytes on ESP32) and is capped at
      // 1,024 pointers (4 KiB). Growth briefly retains both pointer arrays, but
      // moves the owners: the 1 KiB entry chunks themselves are never copied.
      auto grown = allocator_.template allocateArray<std::unique_ptr<Chunk>>(nextSlots);
      if (!grown) return PrepareResult::AllocationFailure;
      for (size_t i = 0; i < directorySlots_; ++i) {
        grown[i] = std::move(directory_[i]);
      }
      directory_ = std::move(grown);
      directorySlots_ = nextSlots;
    }

    directory_[chunkIndex] = std::move(chunk);
    return PrepareResult::Ready;
  }

  // Internal two-phase append contract: callers prepare before doing any
  // externally-visible work, then publish the already-serialized page here.
  void appendPrepared(const Entry& entry) {
    const size_t chunkIndex = size_ / kEntriesPerChunk;
    if (size_ >= kMaxEntries || chunkIndex >= directorySlots_ || !directory_[chunkIndex]) return;
    directory_[chunkIndex]->entries[size_ % kEntriesPerChunk] = entry;
    ++size_;
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  const Entry& operator[](const size_t index) const {
    return directory_[index / kEntriesPerChunk]->entries[index % kEntriesPerChunk];
  }

  void clear() {
    directory_.reset();
    directorySlots_ = 0;
    size_ = 0;
  }

 private:
  struct Chunk {
    Entry entries[kEntriesPerChunk];
  };
  static_assert(sizeof(Chunk) == kEntriesPerChunk * sizeof(Entry), "Section page index chunk has unexpected padding");

  [[no_unique_address]] Allocator allocator_;
  std::unique_ptr<std::unique_ptr<Chunk>[]> directory_;
  size_t directorySlots_ = 0;
  size_t size_ = 0;
};

using SectionPageIndex = BasicSectionPageIndex<>;
