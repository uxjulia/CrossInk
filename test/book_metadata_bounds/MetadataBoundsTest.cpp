// Exercises the production load() with an in-memory file. The firmware
// emulator separately verifies that actual writer output reopens unchanged.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#define LOG_DBG(...)

struct HalFile {
  const std::vector<uint8_t>* bytes = nullptr;
  size_t pos = 0;
  size_t maxReadEnd = 0;
  unsigned closeCount = 0;

  explicit operator bool() const { return bytes != nullptr; }
  size_t position() const { return pos; }
  size_t size() const { return bytes->size(); }
  bool close() {
    bytes = nullptr;
    ++closeCount;
    return true;
  }
  int read(void* out, const size_t len) {
    const size_t n = std::min(len, bytes->size() - pos);
    std::memcpy(out, bytes->data() + pos, n);
    pos += n;
    maxReadEnd = std::max(maxReadEnd, pos);
    return static_cast<int>(n);
  }
};

struct FakeStorage {
  std::vector<uint8_t> book;
  unsigned removals = 0;

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    assert(path == "/cache/book.bin");
    assert(!file);  // SdFat allows only one reader on the same path.
    file.bytes = &book;
    file.pos = file.maxReadEnd = 0;
    return true;
  }
  void remove(const char* path) {
    assert(std::string(path) == "/cache/book.bin");
    ++removals;
  }
} Storage;

namespace OwnedFileSwap {
bool recover(const std::string&) { return true; }
}  // namespace OwnedFileSwap

namespace serialization {
template <typename T>
bool tryReadPod(HalFile& file, T& value) {
  return file.read(&value, sizeof(T)) == sizeof(T);
}
}  // namespace serialization

struct BookMetadataCache {
  std::string cachePath = "/cache";
  uint32_t lutOffset = 0;
  uint16_t spineCount = 0;
  uint16_t tocCount = 0;
  bool loaded = false;
  HalFile bookFile;
  struct {
    std::string title, author, language, coverItemHref, textReferenceHref;
  } coreMetadata;
  std::unique_ptr<uint32_t[]> cumulativeSizes;
  uint16_t cumulativeSizeCount = 0;
  unsigned optionalCalls = 0;
  void cacheSpineCumulativeSizes() { ++optionalCalls; }
  bool load();
};

#include "BoundedMetadataLoad.inc"

template <typename T>
void put(std::vector<uint8_t>& buf, const T item) {
  const size_t old = buf.size();
  buf.resize(old + sizeof(T));
  std::memcpy(buf.data() + old, &item, sizeof(T));
}
template <typename T>
void replace(std::vector<uint8_t>& buf, const size_t offset, const T item) {
  assert(offset + sizeof(T) <= buf.size());
  std::memcpy(buf.data() + offset, &item, sizeof(T));
}

constexpr size_t LUT_OFFSET_FIELD = sizeof(uint32_t) + sizeof(uint8_t);
constexpr size_t METADATA_START = LUT_OFFSET_FIELD + sizeof(uint32_t) + sizeof(uint16_t) * 2;

std::vector<uint8_t> validBook(const std::array<std::string, 5>& fields, const uint16_t spines, const uint16_t toc) {
  std::vector<uint8_t> bytes;
  put(bytes, BOOK_CACHE_MAGIC);
  put(bytes, BOOK_CACHE_VERSION);
  put(bytes, uint32_t{0});
  put(bytes, spines);
  put(bytes, toc);
  for (const auto& s : fields) {
    put(bytes, static_cast<uint32_t>(s.size()));
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  replace(bytes, LUT_OFFSET_FIELD, static_cast<uint32_t>(bytes.size()));
  bytes.resize(bytes.size() + (spines + toc) * sizeof(uint32_t));
  // Leave a minimal spine/TOC body; warm reader validates the header/LUT only.
  bytes.resize(bytes.size() + spines * 10 + toc * 15);
  return bytes;
}

void invalid(std::vector<uint8_t> input) {
  Storage.book = std::move(input);
  Storage.removals = 0;
  BookMetadataCache cache;
  assert(!cache.load() && !cache.loaded && !cache.bookFile);
  assert(cache.optionalCalls == 0 && Storage.removals == 1);
}

int main() {
  const auto valid = validBook({"A title", "", "vi", "Images/côver.png", ""}, 2, 3);
  Storage.book = valid;
  BookMetadataCache cache;
  assert(cache.load() && cache.loaded && cache.optionalCalls == 1);
  assert(cache.coreMetadata.title == "A title" && cache.coreMetadata.author.empty());
  assert(cache.coreMetadata.language == "vi" && cache.coreMetadata.coverItemHref == "Images/côver.png");
  assert(cache.coreMetadata.textReferenceHref.empty());
  assert(cache.bookFile.maxReadEnd == cache.lutOffset && Storage.removals == 0);  // no unconditional LUT or body scan

  auto empty = validBook({}, 0, 0);
  Storage.book = empty;
  assert(cache.load() && cache.loaded && cache.bookFile.closeCount == 1);
  assert(cache.coreMetadata.title.empty());

  auto tooLong = valid;
  replace(tooLong, METADATA_START, uint32_t{300});
  invalid(tooLong);
  for (size_t len : {size_t{3}, size_t{4}, METADATA_START - 1, METADATA_START + 2, METADATA_START + 7}) {
    auto truncated = valid;
    truncated.resize(len);
    invalid(truncated);
  }
  auto insufficientPrefixes = empty;
  replace(insufficientPrefixes, LUT_OFFSET_FIELD, static_cast<uint32_t>(METADATA_START + 4));
  invalid(insufficientPrefixes);
  auto hidesMandatoryLengths = empty;
  replace(hidesMandatoryLengths, METADATA_START, uint32_t{1});
  invalid(hidesMandatoryLengths);
  auto metadataGap = empty;
  metadataGap.push_back(0);
  replace(metadataGap, LUT_OFFSET_FIELD, static_cast<uint32_t>(metadataGap.size()));
  invalid(metadataGap);
  auto bogusOffset = valid;
  replace(bogusOffset, LUT_OFFSET_FIELD, uint32_t{99'999});
  invalid(bogusOffset);
  auto truncatedLut = valid;
  uint32_t validLut = 0;
  std::memcpy(&validLut, valid.data() + LUT_OFFSET_FIELD, sizeof(validLut));
  truncatedLut.resize(validLut + 1);
  invalid(truncatedLut);
  auto bogusCounts = valid;
  replace(bogusCounts, LUT_OFFSET_FIELD + sizeof(uint32_t), uint16_t{2000});
  invalid(bogusCounts);

  // A cache that loaded previously cannot retain its success flag or optional
  // heap when a subsequent malformed file is rejected.
  Storage.book = tooLong;
  cache.cumulativeSizes = std::make_unique<uint32_t[]>(2);
  cache.cumulativeSizeCount = 2;
  assert(!cache.load() && !cache.loaded && !cache.cumulativeSizes && !cache.cumulativeSizeCount);
  assert(cache.optionalCalls == 2 && !cache.bookFile);
}
