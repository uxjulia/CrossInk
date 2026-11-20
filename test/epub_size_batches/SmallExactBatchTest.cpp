// Tests the production small batch and its low-memory and malformed ZIP fallbacks.
#include <ArenaVector.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

template <typename... Args>
void testLog(Args&&...) {}
#define LOG_DBG(...) testLog(__VA_ARGS__)
template <typename T>
std::unique_ptr<T> makeUniqueNoThrow();
enum class MemoryPool { Internal };
struct Heap {
  size_t free;
  size_t largest;
} heap{32768, 24000};
Heap byteHeapSnapshot(MemoryPool) { return heap; }

struct Input {
  std::vector<std::string> items;
  size_t index = 0;
  void seek(size_t position) { index = position; }
};
struct Entry {
  std::string href;
};
Entry readSpineEntryFrom(Input& input) { return {input.items.at(input.index++)}; }
namespace FsHelpers {
std::string normalisePath(const std::string& path) { return path; }
}  // namespace FsHelpers

struct ZipFile {
  struct EntryTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
    const char* path;
  };
  struct EntryIdentity {
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    bool found = false;
  };
  std::vector<std::string> queries;
  unsigned calls = 0;
  unsigned failCall = 0;
  static uint64_t fnvHash64(const char* path, size_t len) { return len == 0 ? 0 : path[0]; }
  int fillEntryIdentities(const EntryTarget* targets, size_t count, EntryIdentity* identities, size_t identitiesCount,
                          size_t limit) {
    ++calls;
    if (calls == failCall) return -1;
    assert(count > 0 && count <= 48 && count == identitiesCount && limit == 196);
    for (size_t i = 0; i < count; ++i) {
      assert(i == 0 || targets[i - 1].hash <= targets[i].hash);
      const auto index = targets[i].index;
      assert(index < identitiesCount && identities[index].uncompressedSize == 0);
      const std::string_view queried(targets[i].path, targets[i].len);
      assert(targets[i].path[targets[i].len] == '\0');
      queries.emplace_back(queried);
      identities[index].uncompressedSize = queried == "zero" || queried == "missing" ? 0 : targets[i].len * 17;
    }
    return static_cast<int>(count);
  }
};
#include "SmallExactBatchConstants.inc"

bool failScratch = false;
unsigned allocCalls = 0;
template <typename T>
std::unique_ptr<T> makeUniqueNoThrow() {
  ++allocCalls;
  return failScratch ? nullptr : std::make_unique<T>();
}

void runSmall(bool expectBatch, size_t failAt = 0, unsigned rejectZipBatch = 0) {
  Input spineIn;
  // 16 short entries cross the target limit after a path-byte-bounded early batch.
  for (int i = 0; i < 16; ++i) spineIn.items.emplace_back(200, 'z' - i);
  for (int i = 16; i < 98; ++i) spineIn.items.push_back(std::to_string(98 - i));
  spineIn.items[20] = "zero";
  spineIn.items[60] = "missing";
  const uint16_t spineCount = spineIn.items.size();
  const bool tryExactSizeBatch = true;

  Arena metadataArena;
  metadataArena.failAllocation = failAt;
  ArenaVector<uint32_t> spineSizes(metadataArena);
  bool useBatchSizes = false;
  ZipFile zip;
  zip.failCall = rejectZipBatch;
#include "SmallExactBatch.inc"
  assert(useBatchSizes == expectBatch);
  if (expectBatch) {
    assert(zip.calls > 2 && zip.queries.size() == spineCount);
    for (size_t i = 0; i < spineSizes.size(); ++i) {
      const auto& path = spineIn.items[i];
      assert(spineSizes[i] == (path == "zero" || path == "missing" ? 0 : path.size() * 17));
    }
  } else if (rejectZipBatch) {
    assert(zip.calls == rejectZipBatch);
  } else {
    assert(zip.calls == 0);
  }
}

int main() {
  ExactSizeBatchPlan plan;
  for (int i = 0; i < 299; ++i) assert(plan.add(std::string(22, 'a')));
  assert(plan.passes == 7);
  ExactSizeBatchPlan longPaths;
  for (int i = 0; i < 64; ++i) assert(longPaths.add(std::string(250, 'a')));
  assert(!longPaths.add(std::string(250, 'a')));
  assert(!ExactSizeBatchPlan{}.add(std::string("a\0b", 3)));
  runSmall(true);
  runSmall(false, 1);     // result allocation failure still allows the legacy reader
  runSmall(false, 0, 2);  // reject even after a successful earlier batch
  failScratch = true;
  runSmall(false);
  failScratch = false;
  allocCalls = 0;
  heap = {4096, 4096};
  runSmall(false);
  heap = {32768, 4096};  // fragmented heap
  runSmall(false);
  assert(allocCalls == 0);
}
