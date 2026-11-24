// The harness includes the production batch block; only storage/ZIP I/O is mocked.
#include <ArenaVector.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#define LOG_ERR(...) (++errors)
template <typename... Args>
void testLog(Args&&...) {}
#define LOG_DBG(...) testLog(__VA_ARGS__)
unsigned errors = 0;
struct Input {
  size_t index = 0;
  size_t count = 0;
  bool failSeek = false;
  bool seek(size_t position) {
    index = position;
    return !failSeek && position <= count;
  }
  size_t position() const { return index; }
} spineIn;
struct Entry {
  std::string href;
};
Entry readSpineEntryFrom(Input& input) {
  // Repeated paths span batches; reverse order exercises sorting.
  return {std::to_string(101 - (input.index++ % 101))};
}
using SpineEntry = Entry;
bool readSpineEntryChecked(Input& input, SpineEntry& entry, size_t fileSize) {
  if (input.position() >= fileSize) return false;
  entry = readSpineEntryFrom(input);
  return true;
}
namespace FsHelpers {
std::string normalisePath(const std::string& path) { return path; }
}  // namespace FsHelpers
struct ZipFile {
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };
  std::vector<size_t> batchSizes;
  std::vector<bool> visited;
  bool closed = false;
  static uint64_t fnvHash64(const char* text, size_t) { return std::stoul(text); }
  int fillUncompressedSizes(const SizeTarget* targets, size_t count, uint32_t* sizes, size_t sizeCount) {
    assert(count > 0 && count <= 2048);
    batchSizes.push_back(count);
    visited.resize(sizeCount);
    int matched = 0;
    for (size_t i = 0; i < count; ++i) {
      assert(i == 0 || targets[i - 1].hash <= targets[i].hash);
      const auto index = targets[i].index;
      assert(index < sizeCount && !visited[index]);
      visited[index] = true;
      // Missing ZIP member preserves the zero initialized result.
      if (targets[i].hash == 50) continue;
      sizes[index] = static_cast<uint32_t>(targets[i].hash * 17);
      ++matched;
    }
    return matched;
  }
  void close() { closed = true; }
};

bool runBatch(uint16_t spineCount, unsigned failAllocation = 0, unsigned failFromAllocation = 0,
              bool failSeek = false) {
  Arena metadataArena;
  metadataArena.failAllocation = failAllocation;
  metadataArena.failFromAllocation = failFromAllocation;
  ArenaVector<uint32_t> spineSizes(metadataArena);
  bool lowMemoryFailure = false, filesClosed = false, useBatchSizes = false;
  auto closeBuildFiles = [&] { filesClosed = true; };
  const auto stopIfCancelled = [] { return false; };
  ZipFile zip;
  constexpr uint16_t LARGE_SPINE_THRESHOLD = 300;
  // Generated directly from BookMetadataCache.cpp, including allocation/cleanup paths.
  auto run = [&]() -> bool {
    spineIn = {};
    spineIn.count = spineCount;
    spineIn.failSeek = failSeek;
    bool inputOk = true;
    bool buildIoFailure = false;
    const size_t spineInputSize = spineCount;
#include "SizeLookup.inc"
    assert(!buildIoFailure);
    return true;
  };
  const bool success = run();
  if (failSeek) {
    assert(!success && filesClosed && zip.closed);
    return false;
  }
  assert(success && !lowMemoryFailure && !filesClosed && !zip.closed);
  if (spineCount < LARGE_SPINE_THRESHOLD) {
    assert(!useBatchSizes && zip.batchSizes.empty());
    return true;
  }
  if (failAllocation == 1) {
    // If the full result vector cannot be allocated, retain the per-entry ZIP
    // fallback rather than making a large book fail to open.
    assert(!useBatchSizes && zip.batchSizes.empty());
    return true;
  }
  if (failFromAllocation != 0) {
    // The result array survived, but even the minimum target batch could not
    // fit. The caller can still use the established per-entry ZIP lookup.
    assert(!useBatchSizes && zip.batchSizes.empty());
    assert(metadataArena.allocations.size() == 7);  // result array plus six attempted batch capacities
    return true;
  }
  assert(useBatchSizes && spineSizes.size() == spineCount);
  const size_t targetCapacity = failAllocation == 2 ? 1024 : 2048;
  assert(zip.batchSizes.size() == (spineCount + targetCapacity - 1) / targetCapacity);
  assert(metadataArena.allocations.size() == (failAllocation == 2 ? 3U : 2U));
  assert(metadataArena.allocations[0] == sizeof(uint32_t) * spineCount);  // full results vector
  assert(metadataArena.allocations[1] <= sizeof(ZipFile::SizeTarget) * 2048);
  if (failAllocation == 2) assert(metadataArena.allocations[2] == sizeof(ZipFile::SizeTarget) * targetCapacity);
  for (size_t i = 0; i < spineCount; ++i) {
    const auto hash = 101 - i % 101;
    assert(zip.visited[i]);
    assert(spineSizes[i] == (hash == 50 ? 0 : hash * 17));
  }
  return true;
}

int main() {
  for (uint16_t count : {299, 300, 2048, 2049, 4096, 5000, 65535}) assert(runBatch(count));
  assert(runBatch(5000, 1));
  assert(runBatch(5000, 2));
  assert(runBatch(5000, 0, 2));
  assert(!runBatch(5000, 0, 0, true));
  assert(errors == 1);  // The injected seek failure logs once and stops cache creation.
}
