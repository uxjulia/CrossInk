#include "ClippingStore.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <uzlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <functional>

#include "activities/reader/ReadingStatsUtils.h"
#include "clippings/ClippingPreview.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t TEXT_OFFSET_VERSION = 2;
constexpr uint8_t LAYOUT_SIGNATURE_VERSION = 3;
constexpr uint8_t VERSION = 4;
constexpr size_t MAX_HEADER_FIELD_BYTES = 4096;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr size_t TEXT_COPY_BUFFER_SIZE = 128;

uint32_t creationTimeUtc() {
  const time_t now = time(nullptr);
  if (now >= 946684800LL && static_cast<uint64_t>(now) <= UINT32_MAX) return static_cast<uint32_t>(now);
  uint16_t year;
  uint8_t month, day, hour, minute;
  if (halClock.getDateTime(year, month, day, hour, minute)) {
    const ReadingStatsDate date{year, month, day};
    if (date.isValid() && hour < 24 && minute < 60) {
      const uint64_t utc =
          946684800ULL + static_cast<uint64_t>(readingStatsDayIndex(date)) * 86400 + hour * 3600U + minute * 60U;
      if (utc <= UINT32_MAX) return static_cast<uint32_t>(utc);
    }
  }
  return 0;  // No clock: preserve an unknown date instead of inventing one.
}

struct ClippingFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
  dst[utf8SafeTruncateBuffer(dst, static_cast<int>(strlen(dst)))] = '\0';
}

// Validate lengths before resizing strings or seeking. Fat files may permit
// seeks beyond EOF, so a successful seek alone does not validate a record.
bool readHeaderString(FsFile& file, std::string* out = nullptr) {
  uint32_t length = 0;
  if (!serialization::tryReadPod(file, length) || length > MAX_HEADER_FIELD_BYTES || file.position() > file.size() ||
      length > file.size() - file.position())
    return false;
  if (!out) return file.seekCur(length);
  out->resize(length);
  return length == 0 || file.read(out->data(), length) == static_cast<int>(length);
}

bool skipText(FsFile& file, uint32_t length) {
  return file.position() <= file.size() && length <= file.size() - file.position() && file.seekCur(length);
}

bool readClippingFileHeader(const std::string& fullPath, const char* name, ClippingFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("CLIP", fullPath, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) ||
      (version != LEGACY_VERSION && version != TEXT_OFFSET_VERSION && version != LAYOUT_SIGNATURE_VERSION &&
       version != VERSION) ||
      !serialization::tryReadPod(f, count) || !readHeaderString(f, &header.title) ||
      !readHeaderString(f, &header.author) || !readHeaderString(f, &header.path)) {
    f.close();
    return false;
  }
  f.close();

  if (count > CLIPPING_MAX_PER_BOOK) {
    return false;
  }
  header.count = count;
  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}

bool copyBytes(FsFile& in, FsFile& out, uint16_t length) {
  std::array<uint8_t, TEXT_COPY_BUFFER_SIZE> buffer{};
  while (length > 0) {
    const size_t chunk = std::min<size_t>(length, buffer.size());
    if (in.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      return false;
    }
    if (out.write(buffer.data(), chunk) != chunk) {
      return false;
    }
    length = static_cast<uint16_t>(length - chunk);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  loaded = false;
  dirty = false;
  clippings.release();
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  pendingSync = {};  // a new reading session for this book
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  storeFilePath = storeFilePathForBook(filePath, bookType);
  const std::string backup = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backup.c_str());
    return false;
  }
  loaded = !Storage.exists(storeFilePath.c_str()) || readFromFile();
  if (!loaded) clippings.release();
  return loaded;
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.release();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
  loaded = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text,
                                                    const uint16_t tableSelection, const uint32_t layoutSignature) {
  if (!loaded) {
    LOG_ERR("CLIP", "Cannot add clipping after failed load");
    return AddResult::SaveFailed;
  }
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  clipping.timestamp = creationTimeUtc();
  clipping.layoutSignature = layoutSignature;
  clipping.tableSelection = tableSelection;
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  const size_t cappedLength = std::min(text.size(), CLIPPING_TEXT_MAX);
  clipping.textLength = static_cast<uint16_t>(utf8SafeTruncateBuffer(text.data(), static_cast<int>(cappedLength)));

  if (!clippings.push_back(clipping)) {
    LOG_ERR("CLIP", "OOM: clipping index");
    return AddResult::SaveFailed;
  }
  dirty = true;
  if (!writeToFile(&text, clippings.size() - 1)) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  dirty = false;
  pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (!loaded || index >= clippings.size()) return false;
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    // unload() may still persist a retried save; queueing the book is harmless.
    pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
    return false;
  }
  pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
  return true;
}

const Clipping* ClippingStore::clippingAt(const size_t index) const {
  if (index >= clippings.size()) return nullptr;
  return &clippings[index];
}

bool ClippingStore::cacheResolvedLayoutRange(const size_t index, const uint16_t page, const uint16_t startWord,
                                             const uint16_t endWord, const uint32_t layoutSignature) {
  if (!loaded || index >= clippings.size()) return false;
  if (!cacheClippingResolvedLayoutRange(clippings[index], page, startWord, endWord, layoutSignature)) {
    return false;
  }
  dirty = true;
  return true;
}

bool ClippingStore::readClippingPreview(const size_t index, std::string& out) const {
  out.clear();
  const Clipping* clipping = clippingAt(index);
  if (!clipping || storeFilePath.empty()) {
    LOG_ERR("CLIP", "Invalid clipping preview index: %u", static_cast<unsigned>(index));
    return false;
  }
  if (clipping->textLength == 0) return true;

  FsFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) return false;
  if (!f.seek(clipping->textOffset)) {
    f.close();
    LOG_ERR("CLIP", "Failed to seek clipping preview at %u", clipping->textOffset);
    return false;
  }
  const bool ok = clippingPreview::read(f, clipping->textLength, out);
  f.close();
  if (!ok) LOG_ERR("CLIP", "Failed to read clipping preview at %u", clipping->textOffset);
  return ok;
}

bool ClippingStore::readClippingText(const size_t index, std::string& out) const {
  const Clipping* clipping = clippingAt(index);
  if (!clipping) return false;
  return readClippingText(*clipping, out);
}

bool ClippingStore::readClippingText(const Clipping& clipping, std::string& out) const {
  out.clear();
  if (clipping.textLength == 0) return true;
  if (storeFilePath.empty()) return false;

  FsFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) {
    return false;
  }
  if (!f.seek(clipping.textOffset)) {
    f.close();
    LOG_ERR("CLIP", "Failed to seek clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
    return false;
  }
  out.resize(clipping.textLength);
  const int expected = static_cast<int>(clipping.textLength);
  const bool ok = f.read(&out[0], clipping.textLength) == expected;
  f.close();
  if (!ok) {
    out.clear();
    LOG_ERR("CLIP", "Failed to read clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
  }
  return ok;
}

bool ClippingStore::saveToFile() {
  if (!loaded) return false;
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

void ClippingStore::clearAll() {
  if (!loaded) {
    LOG_ERR("CLIP", "Cannot clear clippings after failed load");
    return;
  }
  clippings.release();
  dirty = false;
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    Storage.remove(storeFilePath.c_str());
    pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
  }
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, ClippingRecords& out, const size_t onlyIndex) const {
  out.clear();
  bool complete = false;
  struct ClearPartial {
    ClippingRecords& records;
    bool& complete;
    ~ClearPartial() {
      if (!complete) records.release();
    }
  } clearPartial{out, complete};
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;

  if (!serialization::tryReadPod(f, version) ||
      (version != LEGACY_VERSION && version != TEXT_OFFSET_VERSION && version != LAYOUT_SIGNATURE_VERSION &&
       version != VERSION) ||
      !serialization::tryReadPod(f, count) || !readHeaderString(f) || !readHeaderString(f) || !readHeaderString(f)) {
    f.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  if (onlyIndex != SIZE_MAX && onlyIndex >= count) {
    f.close();
    return true;
  }
  if (!out.reserve(onlyIndex == SIZE_MAX ? count : 1)) {
    LOG_ERR("CLIP", "OOM: %u clipping records", count);
    f.close();
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= LAYOUT_SIGNATURE_VERSION && !serialization::tryReadPod(f, clipping.layoutSignature)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at layout signature, record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= VERSION && !serialization::tryReadPod(f, clipping.tableSelection)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at table selection, record %u: %s", i, path.c_str());
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    const int safeTitleLength =
        utf8SafeTruncateBuffer(clipping.chapterTitle, static_cast<int>(strlen(clipping.chapterTitle)));
    clipping.chapterTitle[safeTitleLength] = '\0';
    if (version == LEGACY_VERSION) {
      uint32_t textLen = 0;
      if (!serialization::tryReadPod(f, textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      clipping.textLength = static_cast<uint16_t>(std::min<uint32_t>(textLen, CLIPPING_TEXT_MAX));
      if (textLen > 0 && !skipText(f, textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    } else {
      if (!serialization::tryReadPod(f, clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      if (clipping.textLength > CLIPPING_TEXT_MAX) {
        f.close();
        LOG_ERR("CLIP", "Clipping text length %u exceeds max, record %u: %s", clipping.textLength, i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      if (clipping.textLength > 0 && !skipText(f, clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    }
    if ((onlyIndex == SIZE_MAX || onlyIndex == i) && !out.push_back(clipping)) {
      LOG_ERR("CLIP", "OOM: clipping record");
      f.close();
      return false;
    }
    if (onlyIndex == i) break;
  }

  complete = true;
  f.close();
  return true;
}

bool ClippingStore::readForUpload(const std::string& filePath, const size_t ordinal, Clipping& clipping,
                                  std::string& text, bool& done) {
  done = false;
  ClippingStore reader;
  reader.storeFilePath = storeFilePathForBook(filePath, "epub");
  if (!Storage.exists(reader.storeFilePath.c_str())) {
    reader.storeFilePath += ".bak";
    if (!Storage.exists(reader.storeFilePath.c_str())) {
      done = true;
      return true;
    }
  }
  if (!reader.readFromFile(reader.storeFilePath, reader.clippings, ordinal)) return false;
  if (reader.clippings.empty()) {
    done = true;
    return true;
  }
  clipping = reader.clippings.front();
  return reader.readClippingText(clipping, text);
}

bool ClippingStore::writeToFile(const std::string* replacementText, const size_t replacementIndex,
                                const std::string* sourcePathOverride) {
  if (storeFilePath.empty() || bookTitle.size() > MAX_HEADER_FIELD_BYTES ||
      bookAuthor.size() > MAX_HEADER_FIELD_BYTES || bookFilePath.size() > MAX_HEADER_FIELD_BYTES) {
    LOG_ERR("CLIP", "Invalid clipping store metadata");
    return false;
  }
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  const std::string tmpPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered clipping backup: %s", storeFilePath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(backupPath.c_str()) && Storage.exists(storeFilePath.c_str())) Storage.remove(backupPath.c_str());

  FsFile source;
  const std::string& sourcePath = sourcePathOverride ? *sourcePathOverride : storeFilePath;
  const bool hasSource = Storage.exists(sourcePath.c_str());
  if (hasSource && !Storage.openFileForRead("CLIP", sourcePath, source)) {
    LOG_ERR("CLIP", "Failed to open clipping source for rewrite: %s", sourcePath.c_str());
    return false;
  }

  FsFile f = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (source) source.close();
    LOG_ERR("CLIP", "Failed to open clipping temp file for write: %s", tmpPath.c_str());
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  // Up to 1.5 KiB for rewrite offsets, too large for the firmware task stack.
  auto newTextOffsets = makeUniqueNoThrow<uint32_t[]>(count);
  if (count && !newTextOffsets) {
    LOG_ERR("CLIP", "OOM: clipping rewrite offsets");
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.layoutSignature) ||
        !serialization::tryWritePod(f, clipping.tableSelection) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = replacementText && i == replacementIndex;
    const uint16_t textLen = clipping.textLength;
    if (!serialization::tryWritePod(f, textLen)) {
      LOG_ERR("CLIP", "Failed to write clipping text length %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const uint32_t newTextOffset = static_cast<uint32_t>(f.position());
    bool wroteText = true;
    if (textLen > 0 && useReplacement) {
      wroteText = f.write(reinterpret_cast<const uint8_t*>(replacementText->data()), textLen) == textLen;
    } else if (textLen > 0) {
      wroteText = source && source.seek(clipping.textOffset) && copyBytes(source, f, textLen);
    }
    if (!wroteText) {
      LOG_ERR("CLIP", "Failed to write clipping text %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    newTextOffsets[i] = newTextOffset;
  }

  if (!f.sync()) {
    LOG_ERR("CLIP", "Failed to sync clipping file: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.close();
  if (source) source.close();

  const bool replacingDestination = Storage.exists(storeFilePath.c_str());
  if (replacingDestination && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to replace clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    if (replacingDestination) Storage.rename(backupPath.c_str(), storeFilePath.c_str());
    return false;
  }
  if (replacingDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    clippings[i].textOffset = newTextOffsets[i];
  }
  return true;
}

bool ClippingStore::hasAnyClippings() {
  if (!Storage.exists(CLIPPINGS_DIR)) return false;
  return !Storage.listFiles(CLIPPINGS_DIR, 1).empty();
}

bool ClippingStore::getAllClippedBooks(std::vector<ClippedBookEntry>& out) {
  if (!Storage.exists(CLIPPINGS_DIR)) return true;

  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  for (const auto& name : files) {
    ClippingFileHeader header;
    const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + name.c_str();
    if (!readClippingFileHeader(fullPath, name.c_str(), header)) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == header.bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, header.count);
      continue;
    }
    out.push_back({std::move(header.title), std::move(header.author), std::move(header.path),
                   std::move(header.bookType), header.count});
  }
  return true;
}

ClippingStore::ClippedBookStep ClippingStore::nextClippedEpub(uint64_t& position, std::string& bookPath) {
  if (!Storage.exists(CLIPPINGS_DIR)) return ClippedBookStep::Done;
  auto dir = Storage.open(CLIPPINGS_DIR);
  if (!dir || !dir.isDirectory() || (position && !dir.seekSet(position))) {
    if (dir) dir.close();
    LOG_ERR("CLIP", "Cannot enumerate clippings folder");
    return ClippedBookStep::Error;
  }
  auto entry = dir.openNextFile();
  if (!entry) {
    const bool failed = FsHelpers::directoryIterationFailed(dir);
    dir.close();
    if (failed) {
      LOG_ERR("CLIP", "Clippings folder listing failed");
      return ClippedBookStep::Error;
    }
    return ClippedBookStep::Done;
  }
  position = dir.position();
  char name[64]{};
  const size_t nameLength = entry.getName(name, sizeof(name));
  const bool folder = entry.isDirectory();
  entry.close();
  dir.close();
  // Store files are short "<type>_<crc>.bin" names; anything else is not ours.
  if (folder || nameLength == 0 || nameLength >= sizeof(name) - 1 || strncmp(name, "epub_", 5) != 0 || nameLength < 4 ||
      strcmp(name + nameLength - 4, ".bin") != 0)
    return ClippedBookStep::Skip;
  ClippingFileHeader header;
  const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + name;
  if (!readClippingFileHeader(fullPath, name, header) || header.path.empty() || header.count == 0)
    return ClippedBookStep::Skip;
  if (!Storage.exists(header.path.c_str())) {
    LOG_DBG("CLIP", "Skipping clippings for missing book: %s", header.path.c_str());
    return ClippedBookStep::Skip;
  }
  bookPath = std::move(header.path);
  return ClippedBookStep::Book;
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  ClippingRecords migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = oldStorePath;
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile()) {
    return false;
  }

  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);
  if (oldStorePath == newStorePath) {
    return true;
  }

  const std::string backupPath = newStorePath + ".bak";
  const bool hasDestination = Storage.exists(newStorePath.c_str());
  if (hasDestination) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to remove stale clipping migration backup: %s", backupPath.c_str());
      return false;
    }
    if (!Storage.rename(newStorePath.c_str(), backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to back up destination clippings: %s", newStorePath.c_str());
      return false;
    }
  }
  if (!Storage.rename(oldStorePath.c_str(), newStorePath.c_str())) {
    LOG_ERR("CLIP", "Failed to rename migrated clippings: %s -> %s", oldStorePath.c_str(), newStorePath.c_str());
    if (hasDestination && !Storage.rename(backupPath.c_str(), newStorePath.c_str())) {
      LOG_ERR("CLIP", "Failed to restore destination clipping backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  return true;
}

bool ClippingStore::hasStoredStateForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  constexpr std::array<const char*, 4> suffixes = {"", ".bak", ".tmp", ".rename.bak"};
  return std::any_of(suffixes.begin(), suffixes.end(),
                     [&path](const char* suffix) { return Storage.exists((path + suffix).c_str()); });
}

bool ClippingStore::beginRenameMigration(const std::string& oldFilePath, const std::string& newFilePath,
                                         const std::string& title, const std::string& author,
                                         const std::string& bookType, RenameMigration& migration) {
  migration = {};
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type for rename migration: %s", bookType.c_str());
    return false;
  }
  if (oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) return true;

  migration.sourcePath = storeFilePathForBook(oldFilePath, bookType);
  migration.destinationPath = storeFilePathForBook(newFilePath, bookType);
  migration.destinationBackupPath = migration.destinationPath + ".rename.bak";
  if (!Storage.exists(migration.destinationPath.c_str()) && Storage.exists(migration.destinationBackupPath.c_str())) {
    if (!Storage.rename(migration.destinationBackupPath.c_str(), migration.destinationPath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover interrupted clipping rename backup: %s",
              migration.destinationBackupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered interrupted clipping rename backup: %s", migration.destinationPath.c_str());
  }
  if (!Storage.exists(migration.sourcePath.c_str())) return true;
  if (migration.sourcePath == migration.destinationPath) {
    LOG_ERR("CLIP", "Clipping storage hash collision during rename migration");
    return false;
  }

  ClippingStore reader;
  ClippingRecords migratedClippings;
  if (!reader.readFromFile(migration.sourcePath, migratedClippings)) {
    LOG_ERR("CLIP", "Failed to load source clippings for rename: %s", migration.sourcePath.c_str());
    return false;
  }

  const std::string ordinaryBackupPath = migration.destinationPath + ".bak";
  if (!Storage.exists(migration.destinationPath.c_str()) && Storage.exists(ordinaryBackupPath.c_str()) &&
      !Storage.rename(ordinaryBackupPath.c_str(), migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover destination clippings before rename: %s", ordinaryBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationPath.c_str()) && Storage.exists(ordinaryBackupPath.c_str()) &&
      !Storage.remove(ordinaryBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale destination clipping backup: %s", ordinaryBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationBackupPath.c_str()) &&
      !Storage.remove(migration.destinationBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale clipping rename backup: %s", migration.destinationBackupPath.c_str());
    return false;
  }
  if (Storage.exists(migration.destinationPath.c_str())) {
    if (!Storage.rename(migration.destinationPath.c_str(), migration.destinationBackupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to preserve destination clippings: %s", migration.destinationPath.c_str());
      return false;
    }
    migration.destinationBackedUp = true;
  }

  migration.active = true;
  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = migration.destinationPath;
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile(nullptr, SIZE_MAX, &migration.sourcePath)) {
    LOG_ERR("CLIP", "Failed to write transactional clipping rename: %s", migration.destinationPath.c_str());
    rollbackRenameMigration(migration);
    return false;
  }
  return true;
}

bool ClippingStore::commitRenameMigration(RenameMigration& migration) {
  if (!migration.active) return true;
  bool ok = true;
  if (Storage.exists(migration.sourcePath.c_str()) && !Storage.remove(migration.sourcePath.c_str())) {
    LOG_ERR("CLIP", "Failed to delete renamed clipping source: %s", migration.sourcePath.c_str());
    ok = false;
  }
  if (migration.destinationBackedUp && Storage.exists(migration.destinationBackupPath.c_str()) &&
      !Storage.remove(migration.destinationBackupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to delete clipping rename backup: %s", migration.destinationBackupPath.c_str());
    ok = false;
  }
  migration.active = false;
  return ok;
}

bool ClippingStore::rollbackRenameMigration(RenameMigration& migration) {
  if (!migration.active) return true;
  bool ok = true;
  if (Storage.exists(migration.destinationPath.c_str()) && !Storage.remove(migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove rolled-back clipping destination: %s", migration.destinationPath.c_str());
    ok = false;
  }
  if (migration.destinationBackedUp &&
      !Storage.rename(migration.destinationBackupPath.c_str(), migration.destinationPath.c_str())) {
    LOG_ERR("CLIP", "Failed to restore clipping rename backup: %s", migration.destinationBackupPath.c_str());
    ok = false;
  }
  if (ok) migration.active = false;
  return ok;
}
