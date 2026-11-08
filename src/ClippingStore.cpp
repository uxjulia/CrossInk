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
#include <string_view>

#include "activities/reader/ReadingStatsUtils.h"
#include "clippings/ClippingIdentity.h"
#include "clippings/ClippingPreview.h"
#include "sync/SyncFiles.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t LAYOUT_SIGNATURE_VERSION = 3;
constexpr uint8_t TABLE_SELECTION_VERSION = 4;
constexpr uint8_t SYNC_ID_VERSION = 5;
constexpr uint8_t SCOPED_ID_VERSION = 6;
constexpr uint8_t SOURCE_RANGE_VERSION = 7;
constexpr uint8_t VERSION = SOURCE_RANGE_VERSION;
constexpr size_t SCOPE_BYTES = 16;
constexpr size_t MAX_ID_BYTES = 64;
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

std::string syncPathForStore(const std::string& storePath) {
  return std::string("/.crosspoint/clipping-sync") + storePath.substr(storePath.find_last_of('/'));
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
  if (!serialization::tryReadPod(f, version) || (version < LEGACY_VERSION || version > VERSION) ||
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
// The v5/v6 key immediately precedes the flags, chapter title and text length.
bool readStoredKey(FsFile& file, const Clipping& clip, std::string& key) {
  key.resize(clip.syncKeyLength);
  if (key.empty()) return true;
  const size_t beforeText = key.size() + 1 + sizeof(clip.chapterTitle) + sizeof(clip.textLength);
  return file && clip.textOffset >= beforeText && file.seek(clip.textOffset - beforeText) &&
         file.read(key.data(), key.size()) == static_cast<int>(key.size());
}

bool needsSyncKey(const Clipping& clip) { return !clip.scoped && (!clip.syncKeyLength || clip.uploaded); }

// Bind new selections before their first PUT. Only migration needs to remember
// IDs: v1-v5 selections could share one timestamp/text ID at different locations.
bool bindLocalKey(const std::string& scope, const Clipping& clip, FsFile& source, std::vector<std::string>& legacyIds,
                  std::string& key) {
  if (clip.uploaded) {
    if (key.empty()) {
      std::string text(clip.textLength, '\0');
      char legacy[17];
      if (!source.seek(clip.textOffset) || source.read(text.data(), text.size()) != static_cast<int>(text.size()) ||
          !clippingSyncIdentity(clippingUnixTimestamp(clip.timestamp), text, legacy))
        return false;
      key = legacy;
    }
    if (std::find(legacyIds.begin(), legacyIds.end(), key) == legacyIds.end())
      legacyIds.push_back(key);
    else
      key.clear();
  }
  if (key.empty()) key = newClippingSyncId();
  key.insert(0, scope);
  return true;
}
// Scan on one open SD handle. A matching v5 ID also needs its original passage
// before it can acquire an owner. Unscoped legacy records never match a deletion.
bool findSyncMatch(const std::string& path, const ClippingRecords& clips, const std::string& key,
                   const std::string& text, bool deleted, size_t& index) {
  index = SIZE_MAX;
  if (clips.empty()) return true;
  FsFile file;
  if (!Storage.openFileForRead("CLIP", path, file)) return false;
  std::string saved;
  std::string previousText;
  bool ok = true;
  for (size_t i = 0; i < clips.size(); ++i) {
    const Clipping& clip = clips[i];
    if (!clip.scoped && (deleted || clip.textLength != text.size())) continue;
    ok = readStoredKey(file, clip, saved);
    if (!ok) break;
    if (clip.scoped) {
      if (saved != key) continue;
    } else {
      if (saved.empty()) {
        if (!clip.uploaded) continue;
        char legacy[17];
        ok = clippingSyncIdentity(clippingUnixTimestamp(clip.timestamp), text, legacy);
        if (!ok) break;
        saved = legacy;
      }
      if (std::string_view(key).substr(SCOPE_BYTES) != saved) continue;
      previousText.resize(clip.textLength);
      ok = file.seek(clip.textOffset) &&
           file.read(previousText.data(), previousText.size()) == static_cast<int>(previousText.size());
      if (!ok) break;
      if (previousText == text && index == SIZE_MAX) index = i;
      continue;  // Prefer an already scoped match anywhere in the file.
    }
    index = i;
    break;
  }
  file.close();
  return ok;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  return loadForBook(filePath, title, author, bookType, false);
}

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType, const bool reserveImportSlot) {
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
  deletedIds.clear();
  storeFilePath = storeFilePathForBook(filePath, bookType);
  const std::string backup = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backup.c_str());
    return false;
  }
  loaded =
      !Storage.exists(storeFilePath.c_str()) || readFromFile(storeFilePath, clippings, SIZE_MAX, reserveImportSlot);
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
  std::vector<std::string>().swap(deletedIds);
  loaded = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text,
                                                    const uint16_t tableSelection, const uint32_t layoutSignature,
                                                    const uint32_t sourceStart, const uint32_t sourceEnd) {
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
  if ((sourceStart < sourceEnd && sourceEnd != UINT32_MAX) || (sourceStart == UINT32_MAX && sourceEnd == 0)) {
    clipping.sourceStart = sourceStart;
    clipping.sourceEnd = sourceEnd;
  }
  clipping.tableSelection = tableSelection;
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  const size_t cappedLength = std::min(text.size(), CLIPPING_TEXT_MAX);
  clipping.textLength = static_cast<uint16_t>(utf8SafeTruncateBuffer(text.data(), static_cast<int>(cappedLength)));

  if (!clippings.push_back(clipping)) {
    LOG_ERR("CLIP", "OOM: clipping index");
    return AddResult::SaveFailed;
  }
  dirty = true;
  if (!writeToFile({.index = clippings.size() - 1, .text = &text})) {
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
  const bool needsDeletion = clippings[index].scoped;
  if (needsDeletion && deletedIds.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Sync pending deletions before removing more clippings");
    return false;
  }
  const bool wasDirty = dirty;
  if (needsDeletion) {
    std::string id;
    if (!readKey(clippings[index], id)) return false;
    deletedIds.reserve(deletedIds.size() + 1);
    deletedIds.push_back(id);
  }
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    if (needsDeletion) deletedIds.pop_back();
    dirty = wasDirty;
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

bool ClippingStore::cacheSourceRange(const size_t index, const uint32_t start, const uint32_t end) {
  if (!loaded || index >= clippings.size() || clippingHasSourceRange(clippings[index]) ||
      clippingSourceRangeBlocked(clippings[index]))
    return false;
  auto& clipping = clippings[index];
  if (start == UINT32_MAX && end == 0) {
    clipping.sourceStart = UINT32_MAX;
    clipping.sourceEnd = 0;
    clipping.layoutSignature = 0;  // The saved quote disproved these old word coordinates.
    dirty = true;
    return true;
  }
  if (start != UINT32_MAX) clipping.sourceStart = start;
  if (end != UINT32_MAX) clipping.sourceEnd = end;
  // A cross-page legacy clipping is only ready after both saved endpoints have
  // been visited. Persist at the existing save/unload boundary, never per redraw.
  if (!clippingHasSourceRange(clipping)) return false;
  dirty = true;
  pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
  return true;
}

bool ClippingStore::clippingTextMatchesEdges(const Clipping& clipping, const std::string_view prefix,
                                             const std::string_view suffix, bool& matches) const {
  matches = false;
  if (prefix.size() > clipping.textLength || suffix.size() > clipping.textLength) return true;
  FsFile file;
  if (!Storage.openFileForRead("CLIP", storeFilePath, file)) return false;
  const auto compare = [&](const uint32_t offset, const std::string_view expected) {
    if (expected.empty()) return 1;
    if (!file.seek(offset)) return -1;
    char bytes[64];
    size_t done = 0;
    while (done < expected.size()) {
      const auto count = std::min(sizeof(bytes), expected.size() - done);
      if (file.read(bytes, count) != static_cast<int>(count)) return -1;
      if (memcmp(bytes, expected.data() + done, count) != 0) return 0;
      done += count;
    }
    return 1;
  };
  const int first = compare(clipping.textOffset, prefix);
  const int last = first == 1 ? compare(clipping.textOffset + clipping.textLength - suffix.size(), suffix) : first;
  file.close();
  if (first < 0 || last < 0) {
    LOG_ERR("CLIP", "Could not verify clipping text boundaries");
    return false;
  }
  matches = first == 1 && last == 1;
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
  if (writeToFile({})) {
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
  if (clippings.empty()) return;
  const auto previousCount = deletedIds.size();
  const size_t tombstones =
      std::count_if(clippings.begin(), clippings.end(), [](const Clipping& clip) { return clip.scoped; });
  if (previousCount + tombstones > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Sync pending deletions before clearing clippings");
    return;
  }
  deletedIds.reserve(previousCount + tombstones);
  for (size_t i = 0; i < clippings.size(); ++i) {
    if (!clippings[i].scoped) continue;
    std::string id;
    if (!readKey(clippings[i], id)) {
      deletedIds.resize(previousCount);
      return;
    }
    deletedIds.push_back(std::move(id));
  }
  // One atomic save removes the selections and retains all deletion IDs.
  const bool wasDirty = dirty;
  auto previous = std::move(clippings);
  clippings.clear();
  dirty = true;
  if (!saveToFile()) {
    clippings = std::move(previous);
    deletedIds.resize(previousCount);
    dirty = wasDirty;
    return;
  }
  pendingSync.mark(bookFilePath, SyncPending::CLIPPINGS_BIT);
}

bool ClippingStore::readFromFile(const std::string& path, ClippingRecords& out, const size_t onlyIndex,
                                 const bool reserveImportSlot) {
  out.clear();
  deletedIds.clear();
  bool complete = false;
  struct ClearPartial {
    ClippingRecords& records;
    std::vector<std::string>& deletions;
    bool& complete;
    ~ClearPartial() {
      if (!complete) {
        records.release();
        deletions.clear();
      }
    }
  } clearPartial{out, deletedIds, complete};
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;

  if (!serialization::tryReadPod(f, version) || (version < LEGACY_VERSION || version > VERSION) ||
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

  if (version >= SYNC_ID_VERSION) {
    uint16_t deletedCount = 0;
    if (!serialization::tryReadPod(f, deletedCount) || deletedCount > CLIPPING_MAX_PER_BOOK) {
      f.close();
      return false;
    }
    deletedIds.reserve(deletedCount);
    for (uint16_t i = 0; i < deletedCount; ++i) {
      std::string id;
      uint32_t length = 0;
      if (!serialization::tryReadPod(f, length) || length == 0 ||
          length > MAX_ID_BYTES + (version >= SCOPED_ID_VERSION ? SCOPE_BYTES : 0) ||
          (version >= SCOPED_ID_VERSION && length <= SCOPE_BYTES)) {
        f.close();
        return false;
      }
      id.resize(length);
      if (f.read(id.data(), length) != static_cast<int>(length)) {
        f.close();
        return false;
      }
      // v5 tombstones have no destination. Replaying them under current credentials is unsafe.
      if (version >= SCOPED_ID_VERSION) deletedIds.push_back(std::move(id));
    }
  }

  if (onlyIndex != SIZE_MAX && onlyIndex >= count) {
    // The scope guard reads this through its reference on return.
    // cppcheck-suppress unreadVariable
    complete = true;
    f.close();
    return true;
  }
  // Import needs one additional slot. Reserve it with the initial index so
  // appending never holds two book-sized arrays at once (up to 256 records).
  const size_t capacity = reserveImportSlot ? std::min<size_t>(count + 1, CLIPPING_MAX_PER_BOOK) : count;
  if (!out.reserve(onlyIndex == SIZE_MAX ? capacity : 1)) {
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
    if (version >= TABLE_SELECTION_VERSION && !serialization::tryReadPod(f, clipping.tableSelection)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at table selection, record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= SOURCE_RANGE_VERSION) {
      if (!serialization::tryReadPod(f, clipping.sourceStart) || !serialization::tryReadPod(f, clipping.sourceEnd) ||
          (!clippingHasSourceRange(clipping) && !clippingSourceRangeBlocked(clipping) &&
           (clipping.sourceStart != UINT32_MAX || clipping.sourceEnd != UINT32_MAX))) {
        f.close();
        LOG_ERR("CLIP", "Invalid source range, record %u: %s", i, path.c_str());
        return false;
      }
    }
    if (version >= SYNC_ID_VERSION) {
      uint32_t length = 0;
      if (!serialization::tryReadPod(f, length) || length > MAX_ID_BYTES + SCOPE_BYTES || !skipText(f, length)) {
        f.close();
        return false;
      }
      uint8_t flags = 0;
      if (!serialization::tryReadPod(f, flags) || flags > (version >= SCOPED_ID_VERSION ? 3 : 1)) {
        f.close();
        return false;
      }
      clipping.uploaded = (flags & 1) != 0;
      clipping.scoped = (flags & 2) != 0;
      if (clipping.scoped ? length <= SCOPE_BYTES : length > MAX_ID_BYTES) {
        f.close();
        return false;
      }
      clipping.syncKeyLength = static_cast<uint8_t>(length);
    }
    if (version < SYNC_ID_VERSION) clipping.uploaded = true;
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

  // The scope guard reads this through its reference on return.
  // cppcheck-suppress unreadVariable
  complete = true;
  f.close();
  return true;
}

bool ClippingStore::readForUpload(const std::string& filePath, const size_t ordinal, Clipping& clipping,
                                  std::string& text, bool& done, std::string* key) {
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
  return (!key || reader.readKey(clipping, *key)) && reader.readClippingText(clipping, text);
}

bool ClippingStore::readKey(const Clipping& clip, std::string& key) const {
  key.clear();
  if (!clip.syncKeyLength) return true;
  FsFile file;
  if (!Storage.openFileForRead("CLIP", storeFilePath, file)) return false;
  const bool ok = readStoredKey(file, clip, key);
  file.close();
  return ok;
}

bool ClippingStore::loadForSync(const std::string& path, ClippingStore& store, const bool reserveImportSlot) {
  // Preserve the saved metadata; downloading into a never-opened book can use
  // its filename until the reader supplies the EPUB title/author.
  const auto file = storeFilePathForBook(path, "epub");
  const auto source = Storage.exists(file.c_str()) ? file : file + ".bak";
  ClippingFileHeader header;
  if (Storage.exists(source.c_str()) && !readClippingFileHeader(source, nullptr, header)) return false;
  if (!header.path.empty() && header.path != path) return false;
  const auto title = header.title.empty() ? path.substr(path.find_last_of('/') + 1) : header.title;
  return store.loadForBook(path, title, header.author, "epub", reserveImportSlot);
}

bool ClippingStore::readDeletionForUpload(const std::string& path, const size_t ordinal, std::string& id, bool& done) {
  ClippingStore reader;
  reader.storeFilePath = storeFilePathForBook(path, "epub");
  if (!Storage.exists(reader.storeFilePath.c_str())) reader.storeFilePath += ".bak";
  done = !Storage.exists(reader.storeFilePath.c_str());
  if (done) return true;
  // Read only the header and pending IDs, without a live-record index.
  if (!reader.readFromFile(reader.storeFilePath, reader.clippings, CLIPPING_MAX_PER_BOOK)) return false;
  done = ordinal >= reader.deletedIds.size();
  if (!done) id = reader.deletedIds[ordinal];
  return true;
}

std::string ClippingStore::syncStatePath(const std::string& path) {
  return syncPathForStore(storeFilePathForBook(path, "epub"));
}

bool ClippingStore::prepareUploads(const std::string& path, const std::string& scope) {
  ClippingStore store;
  if (!loadForSync(path, store)) return false;
  if (std::none_of(store.clippings.begin(), store.clippings.end(), needsSyncKey)) return true;
  // A download-only sync may already have bound one of several legacy records
  // with the same ID. Reserve that ID even if its record comes later in the file.
  UploadBinding binding{scope, {}};
  const bool migrating = std::any_of(store.clippings.begin(), store.clippings.end(),
                                     [](const Clipping& clip) { return !clip.scoped && clip.uploaded; });
  if (migrating) {
    binding.legacyIds.reserve(store.clippings.size());
    std::string key;
    for (const auto& clip : store.clippings) {
      if (!clip.scoped) continue;
      if (!store.readKey(clip, key)) return false;
      if (key.starts_with(scope)) binding.legacyIds.push_back(key.substr(SCOPE_BYTES));
    }
    for (const auto& deletion : store.deletedIds)
      if (deletion.starts_with(scope)) binding.legacyIds.push_back(deletion.substr(SCOPE_BYTES));
  }
  return store.writeToFile({.binding = &binding});
}

bool ClippingStore::acknowledgeDeletions(const std::string& path, const std::string& scope) {
  ClippingStore store;
  if (!loadForSync(path, store)) return false;
  std::erase_if(store.deletedIds, [&](const auto& key) { return key.starts_with(scope); });
  return store.writeToFile({});
}

bool ClippingStore::mergeFromSync(const std::string& path, const Clipping& clipping, const std::string& text,
                                  const std::string& key, const bool deleted, bool* full) {
  if (full) *full = false;
  ClippingStore store;
  if (!loadForSync(path, store, !deleted)) return false;
  if (std::find(store.deletedIds.begin(), store.deletedIds.end(), key) != store.deletedIds.end()) return true;
  size_t match;
  if (!findSyncMatch(store.storeFilePath, store.clippings, key, text, deleted, match)) return false;
  if (match != SIZE_MAX) {
    auto& local = store.clippings[match];
    const bool sourceUpgrade = !deleted && !clippingHasSourceRange(local) && clippingHasSourceRange(clipping);
    if (sourceUpgrade) {
      local.sourceStart = clipping.sourceStart;
      local.sourceEnd = clipping.sourceEnd;
    }
    if (!local.scoped) {
      local.scoped = true;
      return store.writeToFile({.index = match, .key = &key});
    }
    if (!deleted) return !sourceUpgrade || store.writeToFile({});
    store.clippings.erase(store.clippings.begin() + match);
    return store.writeToFile({});
  }
  if (deleted) {
    // The empty store anchors the optional cursor even for tombstone-only books.
    return Storage.exists(store.storeFilePath.c_str()) || store.writeToFile({});
  }
  if (store.clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    if (full) *full = true;
    return false;
  }
  if (text.empty() || text.size() > CLIPPING_TEXT_MAX) {
    LOG_ERR("CLIP", "Cannot import clipping: full book or invalid text");
    return false;
  }
  if (!store.clippings.push_back(clipping)) {
    LOG_ERR("CLIP", "OOM: imported clipping index");
    return false;
  }
  auto& added = store.clippings[store.clippings.size() - 1];
  added.scoped = true;
  added.textLength = static_cast<uint16_t>(text.size());
  // Page/word positions belong to the originating device. Source ranges, when
  // provided, identify the passage independently of its layout.
  added.layoutSignature = 0;
  added.tableSelection = UINT16_MAX;
  return store.writeToFile({.index = store.clippings.size() - 1, .text = &text, .key = &key});
}

bool ClippingStore::writeToFile(const Rewrite& rewrite) {
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
  const std::string& sourcePath = rewrite.sourcePath ? *rewrite.sourcePath : storeFilePath;
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
  // Up to 1.25 KiB for rewrite offsets and key lengths, too large for the firmware task stack.
  auto newTextOffsets = makeUniqueNoThrow<uint32_t[]>(count);
  auto newKeyLengths = makeUniqueNoThrow<uint8_t[]>(count);
  if (count && (!newTextOffsets || !newKeyLengths)) {
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

  const uint16_t deletedCount = static_cast<uint16_t>(deletedIds.size());
  bool deletedOk = serialization::tryWritePod(f, deletedCount);
  for (const auto& id : deletedIds) deletedOk = deletedOk && serialization::tryWriteString(f, id);
  if (!deletedOk) {
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  std::string key;
  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    bool keyReady = true;
    if (rewrite.key && rewrite.index == i)
      key = *rewrite.key;
    else
      keyReady = readStoredKey(source, clipping, key);
    const bool bind = rewrite.binding && needsSyncKey(clipping);
    if (keyReady && bind)
      keyReady = bindLocalKey(rewrite.binding->scope, clipping, source, rewrite.binding->legacyIds, key);
    const uint8_t flags = bind ? 3 : (clipping.uploaded ? 1 : 0) | (clipping.scoped ? 2 : 0);
    if (!keyReady || !serialization::tryWritePod(f, clipping.spineIndex) ||
        !serialization::tryWritePod(f, clipping.startPage) || !serialization::tryWritePod(f, clipping.endPage) ||
        !serialization::tryWritePod(f, clipping.pageCount) || !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.layoutSignature) ||
        !serialization::tryWritePod(f, clipping.tableSelection) ||
        !serialization::tryWritePod(f, clippingHasSourceRange(clipping) ? clipping.sourceStart : UINT32_MAX) ||
        !serialization::tryWritePod(f, clippingSourceRangeBlocked(clipping)
                                           ? 0U
                                           : (clippingHasSourceRange(clipping) ? clipping.sourceEnd : UINT32_MAX)) ||
        !serialization::tryWriteString(f, key) || !serialization::tryWritePod(f, flags) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = rewrite.text && i == rewrite.index;
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
      wroteText = f.write(reinterpret_cast<const uint8_t*>(rewrite.text->data()), textLen) == textLen;
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
    newKeyLengths[i] = key.size();
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
    if (rewrite.binding && needsSyncKey(clippings[i])) clippings[i].scoped = clippings[i].uploaded = true;
    clippings[i].syncKeyLength = newKeyLengths[i];
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
  if (!readClippingFileHeader(fullPath, name, header) || header.path.empty()) return ClippedBookStep::Skip;
  if (!Storage.exists(header.path.c_str())) {
    LOG_DBG("CLIP", "Skipping clippings for missing book: %s", header.path.c_str());
    return ClippedBookStep::Skip;
  }
  bookPath = std::move(header.path);
  return ClippedBookStep::Book;
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  SyncFiles::removeAll(syncStatePath(filePath).c_str());
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
  writer.deletedIds = std::move(reader.deletedIds);
  if (!writer.writeToFile({})) {
    return false;
  }

  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);
  if (oldStorePath == newStorePath) {
    return true;
  }

  SyncFiles::removeAll(syncPathForStore(newStorePath).c_str());
  SyncFiles::removeAll(syncPathForStore(oldStorePath).c_str());
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

  SyncFiles::removeAll(syncPathForStore(migration.destinationPath).c_str());
  migration.active = true;
  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = migration.destinationPath;
  writer.clippings = std::move(migratedClippings);
  writer.deletedIds = std::move(reader.deletedIds);
  if (!writer.writeToFile({.sourcePath = &migration.sourcePath})) {
    LOG_ERR("CLIP", "Failed to write transactional clipping rename: %s", migration.destinationPath.c_str());
    rollbackRenameMigration(migration);
    return false;
  }
  return true;
}

bool ClippingStore::commitRenameMigration(RenameMigration& migration) {
  if (!migration.active) return true;
  SyncFiles::removeAll(syncPathForStore(migration.sourcePath).c_str());
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
