#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

constexpr size_t MAX_BUILD_ENTRY_STRING_BYTES = 16 * 1024;
constexpr char bookBinFile[] = "/book.bin";

struct FakeFile {
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  size_t failAt = std::numeric_limits<size_t>::max();
  bool failSeek = false;
  bool active = true;

  explicit operator bool() const { return active; }
  size_t position() const { return cursor; }
  size_t size() const { return bytes.size(); }
  bool seek(size_t position) {
    if (!active || failSeek || position > bytes.size()) return false;
    cursor = position;
    return true;
  }
  bool seekCur(size_t length) {
    return cursor <= bytes.size() && length <= bytes.size() - cursor && seek(cursor + length);
  }
  int read(uint8_t* destination, size_t length) {
    if (!active || cursor > bytes.size()) return -1;
    if (cursor == failAt) {
      const size_t shortLength = length == 0 ? 0 : length - 1;
      std::memcpy(destination, bytes.data() + cursor, shortLength);
      cursor += shortLength;
      return static_cast<int>(shortLength);
    }
    const size_t actual = std::min(length, bytes.size() - cursor);
    std::memcpy(destination, bytes.data() + cursor, actual);
    cursor += actual;
    return static_cast<int>(actual);
  }
  bool close() {
    active = false;
    return true;
  }
};

struct FakeStorage {
  unsigned removals = 0;
  FakeFile* cacheFile = nullptr;

  bool remove(const char* path) {
    assert(std::string(path) == "/cache/book.bin");
    assert(cacheFile && !cacheFile->active);
    ++removals;
    return true;
  }
} Storage;

#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)

class BookMetadataCache {
 public:
  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize = 0;
    int16_t tocIndex = -1;
  };
  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level = 0;
    int16_t spineIndex = -1;
  };

  std::string cachePath = "/cache";
  uint32_t lutOffset = 4;
  uint16_t spineCount = 1;
  uint16_t tocCount = 1;
  bool loaded = true;
  FakeFile bookFile;
  std::unique_ptr<uint32_t[]> cumulativeSizes;
  uint16_t cumulativeSizeCount = 0;

  SpineEntry getSpineEntry(int index);
  size_t getSpineCumulativeSize(int index);
  TocEntry getTocEntry(int index);
  bool readEntryBounds(bool spineEntry, int index, size_t& start, size_t& end, bool& ioFailure);
  void invalidateReadCache(bool discardCache);
};

#include "CheckedMetadataReads.inc"

struct BufferedLikeFile {
  FakeFile file;
  size_t position() const { return file.position(); }
  size_t read(uint8_t* destination, size_t length) { return static_cast<size_t>(file.read(destination, length)); }
};

template <typename T>
void append(FakeFile& file, const T value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  file.bytes.insert(file.bytes.end(), bytes, bytes + sizeof(T));
}

void appendString(FakeFile& file, const std::string& value) {
  append(file, static_cast<uint32_t>(value.size()));
  file.bytes.insert(file.bytes.end(), value.begin(), value.end());
}

void writeAt(FakeFile& file, size_t offset, uint32_t value) {
  std::memcpy(file.bytes.data() + offset, &value, sizeof(value));
}

void makeValidCache(BookMetadataCache& cache) {
  cache.bookFile.bytes.assign(4, 0);  // Metadata precedes the lookup table.
  append(cache.bookFile, uint32_t{0});
  append(cache.bookFile, uint32_t{0});
  const uint32_t spinePosition = cache.bookFile.bytes.size();
  appendString(cache.bookFile, "chapter.xhtml");
  append(cache.bookFile, uint32_t{4096});
  append(cache.bookFile, int16_t{0});
  const uint32_t tocPosition = cache.bookFile.bytes.size();
  appendString(cache.bookFile, "Chapter One");
  appendString(cache.bookFile, "chapter.xhtml");
  appendString(cache.bookFile, "start");
  append(cache.bookFile, uint8_t{1});
  append(cache.bookFile, int16_t{0});
  writeAt(cache.bookFile, cache.lutOffset, spinePosition);
  writeAt(cache.bookFile, cache.lutOffset + sizeof(uint32_t), tocPosition);
  cache.bookFile.cursor = 0;
  cache.bookFile.active = true;
  Storage.removals = 0;
  Storage.cacheFile = &cache.bookFile;
}

int main() {
  FakeFile spineFile;
  appendString(spineFile, "Text/chapter.xhtml");
  append(spineFile, uint32_t{4096});
  append(spineFile, int16_t{7});
  BookMetadataCache::SpineEntry spine;
  assert(readSpineEntryChecked(spineFile, spine, spineFile.bytes.size()));
  assert(spine.href == "Text/chapter.xhtml" && spine.cumulativeSize == 4096 && spine.tocIndex == 7);

  FakeFile shortRead;
  appendString(shortRead, "chapter.xhtml");
  shortRead.failAt = sizeof(uint32_t);
  BookMetadataCache::SpineEntry ignoredSpine;
  assert(!readSpineEntryChecked(shortRead, ignoredSpine, shortRead.bytes.size()));

  FakeFile shortPod;
  append(shortPod, uint32_t{17});
  shortPod.failAt = 0;
  BookMetadataCache::SpineEntry ignoredPod;
  assert(!readSpineEntryChecked(shortPod, ignoredPod, shortPod.bytes.size()));

  FakeFile impossibleLength;
  append(impossibleLength, uint32_t{std::numeric_limits<uint32_t>::max()});
  BookMetadataCache::SpineEntry oversized;
  assert(!readSpineEntryChecked(impossibleLength, oversized, impossibleLength.bytes.size()));
  assert(oversized.href.empty());

  FakeFile unreasonableLength;
  append(unreasonableLength, uint32_t{MAX_BUILD_ENTRY_STRING_BYTES + 1});
  BookMetadataCache::SpineEntry tooLarge;
  assert(!readSpineEntryChecked(unreasonableLength, tooLarge, unreasonableLength.bytes.size()));

  FakeFile tocFile;
  appendString(tocFile, "Chapter One");
  appendString(tocFile, "Text/chapter.xhtml");
  appendString(tocFile, "start");
  append(tocFile, uint8_t{2});
  append(tocFile, int16_t{0});
  BookMetadataCache::TocEntry toc;
  assert(readTocEntryChecked(tocFile, toc, tocFile.bytes.size()));
  assert(toc.title == "Chapter One" && toc.href == "Text/chapter.xhtml" && toc.anchor == "start");
  assert(toc.level == 2 && toc.spineIndex == 0);

  BufferedLikeFile buffered;
  appendString(buffered.file, "short");
  append(buffered.file, uint32_t{1});
  buffered.file.failAt = sizeof(uint32_t);
  BookMetadataCache::SpineEntry bufferedEntry;
  assert(!readSpineEntryChecked(buffered, bufferedEntry, buffered.file.bytes.size()));

  BookMetadataCache valid;
  makeValidCache(valid);
  auto validSpine = valid.getSpineEntry(0);
  assert(valid.loaded && validSpine.href == "chapter.xhtml" && validSpine.cumulativeSize == 4096);
  assert(valid.getSpineCumulativeSize(0) == 4096 && valid.loaded);
  auto validToc = valid.getTocEntry(0);
  assert(valid.loaded && validToc.title == "Chapter One" && validToc.spineIndex == 0);
  assert(Storage.removals == 0);

  BookMetadataCache truncated;
  makeValidCache(truncated);
  truncated.bookFile.bytes.pop_back();
  assert(truncated.getTocEntry(0).title.empty());
  assert(!truncated.loaded && Storage.removals == 1 && !truncated.bookFile.active);

  BookMetadataCache shortLiveRead;
  makeValidCache(shortLiveRead);
  shortLiveRead.bookFile.failAt = 12 + sizeof(uint32_t);
  assert(shortLiveRead.getSpineEntry(0).href.empty());
  assert(!shortLiveRead.loaded && Storage.removals == 0 && !shortLiveRead.bookFile.active);

  BookMetadataCache badLut;
  makeValidCache(badLut);
  writeAt(badLut.bookFile, badLut.lutOffset, 1000);
  assert(badLut.getSpineEntry(0).href.empty());
  assert(!badLut.loaded && Storage.removals == 1);

  BookMetadataCache failedSeek;
  makeValidCache(failedSeek);
  failedSeek.bookFile.failSeek = true;
  assert(failedSeek.getSpineEntry(0).href.empty());
  assert(!failedSeek.loaded && Storage.removals == 0);
}
