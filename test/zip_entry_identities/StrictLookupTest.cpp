// The strict lookup implementation is extracted from production; only the
// central directory and open/close seams are simulated.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define LOG_ERR(...)
constexpr uint16_t ZIP_MAX_STORED_NAME_LEN = 255;
struct Item {
  std::string name;
  uint32_t size;
};
struct CentralDirEntry {
  uint16_t nameLen;
  bool nameStored;
  uint32_t crc32;
  uint32_t compressedSize;
  uint32_t uncompressedSize;
};
struct CentralDirCursor {
  const std::vector<Item>& items;
  uint32_t start;
  uint32_t count = 0;
  CentralDirCursor(const std::vector<Item>& items, uint32_t start) : items(items), start(start) {}
  bool valid() const { return true; }
  uint32_t position() const { return start + count; }
  bool next(CentralDirEntry& entry, char* name) {
    if (count == items.size()) return false;
    const auto& item = items[count++];
    entry = {static_cast<uint16_t>(item.name.size()), item.name.size() <= ZIP_MAX_STORED_NAME_LEN, 42, 1, item.size};
    if (entry.nameStored) std::memcpy(name, item.name.c_str(), item.name.size() + 1);
    return true;
  }
};
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
  struct ZipDetails {
    uint32_t totalEntries;
    uint32_t centralDirOffset;
    uint32_t centralDirSize;
    size_t eocdOffset;
  };
  std::vector<Item> file;
  ZipDetails zipDetails;
  explicit ZipFile(std::vector<Item> items)
      : file(std::move(items)),
        zipDetails{static_cast<uint32_t>(file.size()), 100, static_cast<uint32_t>(file.size()), 100 + file.size()} {}
  bool loadZipDetails() { return true; }
  static uint64_t fnvHash64(const char*, size_t) { return 0; }  // every name collides deliberately
  int fillEntryIdentities(const EntryTarget*, size_t, EntryIdentity*, size_t, size_t exactScanEntryLimit = 0);
};
struct ScopedOpenClose {
  explicit ScopedOpenClose(ZipFile&) {}
  explicit operator bool() const { return true; }
};

#include "ProductionStrictLookup.inc"

int main() {
  using Target = ZipFile::EntryTarget;
  using Identity = ZipFile::EntryIdentity;
  const Target targets[] = {{0, 3, 0, "one"}, {0, 3, 1, "one"}, {0, 3, 2, "two"}, {0, 3, 3, "zzz"}};
  Identity identities[4];
  ZipFile unique({{"won", 78}, {"two", 0}, {"one", 123}, {std::string(256, 'x'), 7}});
  assert(unique.fillEntryIdentities(targets, 4, identities, 4, 4) == 3);
  assert(identities[0].found && identities[0].uncompressedSize == 123);
  assert(identities[1].found && identities[1].uncompressedSize == 123);
  assert(identities[2].found && identities[2].uncompressedSize == 0);
  assert(!identities[3].found && identities[3].uncompressedSize == 0);

  std::fill_n(identities, 4, Identity{});
  assert(unique.fillEntryIdentities(targets, 4, identities, 4, 3) == -1);

  // A duplicate after all the targets are seen must not bypass rejection.
  const Target one{0, 3, 0, "one"};
  Identity first;
  ZipFile duplicate({{"one", 123}, {"other", 456}, {"one", 789}});
  assert(duplicate.fillEntryIdentities(&one, 1, &first, 1) == 1 && first.uncompressedSize == 123);
  first = {};
  assert(duplicate.fillEntryIdentities(&one, 1, &first, 1, 4) == -1);

  duplicate.zipDetails.totalEntries = 2;
  first = {};
  assert(duplicate.fillEntryIdentities(&one, 1, &first, 1, 4) == -1);

  duplicate.zipDetails.centralDirSize = 2;
  first = {};
  assert(duplicate.fillEntryIdentities(&one, 1, &first, 1, 4) == -1);

  ZipFile truncated({{"one", 123}});
  truncated.zipDetails.totalEntries += 1;
  first = {};
  assert(truncated.fillEntryIdentities(&one, 1, &first, 1, 4) == -1);

  ZipFile nul({{"one", 0}, {std::string("one\0d", 5), 456}});
  first = {};
  assert(nul.fillEntryIdentities(&one, 1, &first, 1, 4) == -1);
}
