#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <functional>
#include <limits>

namespace {
constexpr uint8_t LEGACY_VERSION = 2;
constexpr uint8_t COUNT_U16_VERSION = 3;
constexpr uint8_t PARAGRAPH_ANCHOR_VERSION = 4;
constexpr uint8_t VERSION = 5;
// Stored count is uint16_t in v3+, but we keep an in-memory safety cap for ESP32-C3 RAM.
constexpr uint16_t MAX_BOOKMARKS = 1024;
constexpr size_t INITIAL_BOOKMARK_RESERVE = 8;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";
constexpr char READ_FOLDER[] = "/Read";

struct BookmarkFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

std::string currentStoreFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

std::string legacyStoreFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(std::hash<std::string>{}(filePath)) +
         ".bin";
}

bool readBookmarkCount(FsFile& file, const uint8_t version, uint16_t& count) {
  if (version == LEGACY_VERSION) {
    uint8_t legacyCount = 0;
    serialization::readPod(file, legacyCount);
    count = legacyCount;
    return true;
  }

  if (version == COUNT_U16_VERSION || version == PARAGRAPH_ANCHOR_VERSION || version == VERSION) {
    serialization::readPod(file, count);
    return true;
  }

  return false;
}

bool bookmarksMatchIdentity(const Bookmark& a, const Bookmark& b) {
  return a.spineIndex == b.spineIndex && a.progress == b.progress;
}

bool mergeBookmarks(std::vector<Bookmark>& dst, const std::vector<Bookmark>& src) {
  bool mergedAny = false;
  for (const auto& bookmark : src) {
    const auto it = std::find_if(dst.begin(), dst.end(),
                                 [&](const Bookmark& existing) { return bookmarksMatchIdentity(existing, bookmark); });
    if (it != dst.end()) {
      continue;
    }
    if (dst.size() >= MAX_BOOKMARKS) {
      LOG_ERR("BKS", "Bookmark limit (%u) reached while merging legacy bookmarks", MAX_BOOKMARKS);
      break;
    }
    dst.push_back(bookmark);
    mergedAny = true;
  }
  return mergedAny;
}

bool deleteBookmarkStorePath(const std::string& path, const std::string& reasonTag) {
  if (!Storage.exists(path.c_str())) {
    return true;
  }
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("BKS", "Failed to delete %s bookmark file: %s", reasonTag.c_str(), path.c_str());
    return false;
  }
  return true;
}

std::string fileNameFromPath(const std::string& path) {
  const size_t lastSlash = path.rfind('/');
  return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

bool isReadFolderCollisionVariant(const std::string& originalBase, const std::string& candidateBase) {
  if (candidateBase.size() <= originalBase.size() + 4) {
    return false;
  }
  if (candidateBase.compare(0, originalBase.size(), originalBase) != 0) {
    return false;
  }
  if (candidateBase.compare(originalBase.size(), 2, " (") != 0 || candidateBase.back() != ')') {
    return false;
  }
  for (size_t i = originalBase.size() + 2; i + 1 < candidateBase.size(); i++) {
    if (candidateBase[i] < '0' || candidateBase[i] > '9') {
      return false;
    }
  }
  return true;
}

bool resolveMovedToReadDestinationPath(const std::string& originalPath, std::string& resolvedPath) {
  const std::string fileName = fileNameFromPath(originalPath);
  if (fileName.empty() || !Storage.exists(READ_FOLDER)) {
    return false;
  }

  const std::string exactPath = std::string(READ_FOLDER) + "/" + fileName;
  if (Storage.exists(exactPath.c_str())) {
    resolvedPath = exactPath;
    return true;
  }

  const size_t dotPos = fileName.rfind('.');
  const std::string originalBase = (dotPos != std::string::npos) ? fileName.substr(0, dotPos) : fileName;
  const std::string originalExt = (dotPos != std::string::npos) ? fileName.substr(dotPos) : "";

  std::string matchedPath;
  size_t matchCount = 0;
  for (const auto& entry : Storage.listFiles(READ_FOLDER)) {
    const std::string candidateName = entry.c_str();
    const size_t candidateDotPos = candidateName.rfind('.');
    const std::string candidateBase =
        (candidateDotPos != std::string::npos) ? candidateName.substr(0, candidateDotPos) : candidateName;
    const std::string candidateExt =
        (candidateDotPos != std::string::npos) ? candidateName.substr(candidateDotPos) : "";

    if (candidateExt != originalExt || !isReadFolderCollisionVariant(originalBase, candidateBase)) {
      continue;
    }

    matchedPath = std::string(READ_FOLDER) + "/" + candidateName;
    matchCount++;
    if (matchCount > 1) {
      return false;
    }
  }

  if (matchCount == 1) {
    resolvedPath = matchedPath;
    return true;
  }
  return false;
}

bool readBookmarkFileHeader(const std::string& fullPath, const char* name, BookmarkFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("BKS", fullPath, f)) {
    return false;
  }

  if (f.available() < static_cast<int>(sizeof(uint8_t))) {
    f.close();
    return false;
  }
  uint8_t version;
  serialization::readPod(f, version);
  if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
      version != VERSION) {
    f.close();
    return false;
  }

  if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
    f.close();
    return false;
  }
  if (!readBookmarkCount(f, version, header.count)) {
    f.close();
    return false;
  }

  auto readCheckedString = [&f](std::string& s) -> bool {
    uint32_t len;
    if (f.available() < static_cast<int>(sizeof(len))) return false;
    serialization::readPod(f, len);
    if (f.available() < static_cast<int>(len)) return false;
    s.resize(len);
    f.read(reinterpret_cast<uint8_t*>(&s[0]), len);
    return true;
  };

  if (!readCheckedString(header.title) || !readCheckedString(header.author) || !readCheckedString(header.path)) {
    f.close();
    return false;
  }
  f.close();

  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}
}  // namespace

BookmarkStore BookmarkStore::instance;

bool BookmarkStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  bookmarks.clear();
  if (bookmarks.capacity() < INITIAL_BOOKMARK_RESERVE) {
    bookmarks.reserve(INITIAL_BOOKMARK_RESERVE);
  }

  storeFilePath = currentStoreFilePathForBook(filePath, bookType);
  const std::string legacyStoreFilePath = legacyStoreFilePathForBook(filePath, bookType);
  const bool hasCurrentFile = Storage.exists(storeFilePath.c_str());
  const bool hasLegacyFile = legacyStoreFilePath != storeFilePath && Storage.exists(legacyStoreFilePath.c_str());

  if (!hasCurrentFile && !hasLegacyFile) {
    if (bookType == "epub" && isInReadFolder(filePath) && Storage.exists(BOOKMARKS_DIR)) {
      for (const auto& name : Storage.listFiles(BOOKMARKS_DIR)) {
        BookmarkFileHeader header;
        const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();
        if (!readBookmarkFileHeader(fullPath, name.c_str(), header)) continue;
        if (header.bookType != bookType || header.count == 0 || Storage.exists(header.path.c_str())) continue;
        if (!title.empty() && !header.title.empty() && header.title != title) continue;
        if (!author.empty() && !header.author.empty() && header.author != author) continue;

        std::string resolvedMovedPath;
        if (!resolveMovedToReadDestinationPath(header.path, resolvedMovedPath) || resolvedMovedPath != filePath) {
          continue;
        }

        if (migrateForFilePath(header.path, filePath, title, author, bookType)) {
          return loadForBook(filePath, title, author, bookType);
        }
        break;
      }
    }

    LOG_DBG("BKS", "No bookmark file for this book");
    return true;
  }

  bool loadedAny = false;
  bool needsRewrite = false;

  bool currentLoaded = false;
  if (hasCurrentFile) {
    bool currentNeedsRewrite = false;
    if (readFromFile(storeFilePath, bookmarks, currentNeedsRewrite)) {
      loadedAny = true;
      currentLoaded = true;
      needsRewrite = currentNeedsRewrite;
    } else {
      LOG_ERR("BKS", "Failed to load canonical bookmark file: %s", storeFilePath.c_str());
    }
  }

  if (hasLegacyFile) {
    bool legacyNeedsRewrite = false;
    std::vector<Bookmark> legacyBookmarks;
    if (readFromFile(legacyStoreFilePath, legacyBookmarks, legacyNeedsRewrite)) {
      const bool mergedLegacyBookmarks = mergeBookmarks(bookmarks, legacyBookmarks);
      loadedAny = true;
      bool canDeleteLegacyFile = currentLoaded;

      if (!hasCurrentFile || mergedLegacyBookmarks || legacyNeedsRewrite || needsRewrite) {
        dirty = true;
        saveToFile();
        canDeleteLegacyFile = !dirty;
      }

      if (canDeleteLegacyFile) {
        if (deleteBookmarkStorePath(legacyStoreFilePath, "legacy")) {
          LOG_INF("BKS", "Migrated legacy bookmark store: %s -> %s", legacyStoreFilePath.c_str(),
                  storeFilePath.c_str());
        }
      }
    } else {
      LOG_ERR("BKS", "Failed to load legacy bookmark file: %s", legacyStoreFilePath.c_str());
    }
  }

  if (!loadedAny) {
    return false;
  }

  if (needsRewrite && !hasLegacyFile) {
    dirty = true;
    saveToFile();
  }

  return true;
}

void BookmarkStore::unload() {
  if (dirty) saveToFile();
  bookmarks.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

BookmarkStore::AddResult BookmarkStore::addBookmark(uint16_t spineIndex, float progress, int pageCount,
                                                    const char* chapterTitle, uint16_t paragraphIndex,
                                                    const char* snippet) {
  if (pageCount > 0) {
    const float pageSlice = 1.0f / static_cast<float>(pageCount);
    const float pageStart = progress;
    const float pageEnd = progress + pageSlice;
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
    });
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.timestamp = 0;  // ESP32-C3 has no battery-backed RTC; reserved for future use
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bm.paragraphIndex = paragraphIndex;
  snprintf(bm.snippet, sizeof(bm.snippet), "%s", snippet ? snippet : "");

  bookmarks.push_back(bm);
  dirty = true;
  saveToFile();
  return AddResult::Added;
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
  if (it == bookmarks.end()) return;

  bookmarks.erase(it);
  dirty = true;
  saveToFile();
}

bool BookmarkStore::removeBookmarkAt(size_t index) {
  if (index >= bookmarks.size()) return false;

  bookmarks.erase(bookmarks.begin() + index);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

void BookmarkStore::saveToFile() {
  if (!dirty || storeFilePath.empty()) return;
  if (bookmarks.empty()) {
    if (Storage.exists(storeFilePath.c_str())) Storage.remove(storeFilePath.c_str());
    dirty = false;
    return;
  }
  if (writeToFile()) dirty = false;
}

void BookmarkStore::clearAll() {
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    if (!Storage.remove(storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to delete bookmark file");
      return;
    }
    LOG_DBG("BKS", "Bookmark file deleted");
  }
  bookmarks.clear();
  dirty = false;
}

bool BookmarkStore::readFromFile() {
  bool needsRewrite = false;
  if (!readFromFile(storeFilePath, bookmarks, needsRewrite)) {
    return false;
  }

  if (needsRewrite) {
    dirty = true;
    saveToFile();
    LOG_DBG("BKS", "Migrated bookmark file to version %u", VERSION);
  }
  LOG_DBG("BKS", "Loaded %u bookmark(s)", static_cast<unsigned>(bookmarks.size()));
  return true;
}

bool BookmarkStore::readFromFile(const std::string& path, std::vector<Bookmark>& out, bool& needsRewrite) const {
  needsRewrite = false;
  FsFile f;
  if (!Storage.openFileForRead("BKS", path, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for read: %s", path.c_str());
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
      version != VERSION) {
    LOG_ERR("BKS", "Unknown bookmark file version %u: %s", version, path.c_str());
    f.close();
    return false;
  }

  uint16_t count = 0;
  if (!readBookmarkCount(f, version, count)) {
    LOG_ERR("BKS", "Failed to read bookmark count for version %u: %s", version, path.c_str());
    f.close();
    return false;
  }
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  std::string tmp;
  serialization::readString(f, tmp);  // title — not validated
  serialization::readString(f, tmp);  // author — not validated
  std::string storedPath;
  serialization::readString(f, storedPath);
  if (storedPath != bookFilePath) {
    LOG_ERR("BKS", "Bookmark file path mismatch, file may belong to a different book: %s", path.c_str());
    f.close();
    return false;
  }

  std::vector<Bookmark> loadedBookmarks;
  loadedBookmarks.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Bookmark bm{};
    if (f.available() < static_cast<int>(sizeof(bm.spineIndex))) {
      LOG_ERR("BKS", "Bookmark file truncated at spineIndex, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.spineIndex);
    if (f.available() < static_cast<int>(sizeof(bm.progress))) {
      LOG_ERR("BKS", "Bookmark file truncated at progress, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.progress);
    if (f.available() < static_cast<int>(sizeof(bm.timestamp))) {
      LOG_ERR("BKS", "Bookmark file truncated at timestamp, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.timestamp);
    const int chRead = f.read(bm.chapterTitle, sizeof(bm.chapterTitle));
    bm.chapterTitle[sizeof(bm.chapterTitle) - 1] = '\0';
    if (chRead != static_cast<int>(sizeof(bm.chapterTitle))) {
      LOG_ERR("BKS", "Bookmark file truncated at chapterTitle, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    if (version >= PARAGRAPH_ANCHOR_VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.paragraphIndex))) {
        LOG_ERR("BKS", "Bookmark file truncated at paragraphIndex, record %u: %s", i, path.c_str());
        f.close();
        return false;
      }
      serialization::readPod(f, bm.paragraphIndex);
    } else {
      bm.paragraphIndex = UINT16_MAX;
    }
    if (version >= VERSION) {
      const int snippetRead = f.read(bm.snippet, sizeof(bm.snippet));
      bm.snippet[sizeof(bm.snippet) - 1] = '\0';
      if (snippetRead != static_cast<int>(sizeof(bm.snippet))) {
        LOG_ERR("BKS", "Bookmark file truncated at snippet, record %u: %s", i, path.c_str());
        f.close();
        return false;
      }
    } else {
      bm.snippet[0] = '\0';
    }
    loadedBookmarks.push_back(bm);
  }

  f.close();
  out = std::move(loadedBookmarks);
  needsRewrite = version != VERSION;
  return true;
}

bool BookmarkStore::writeToFile() const {
  Storage.mkdir(BOOKMARKS_DIR);

  FsFile f;
  if (!Storage.openFileForWrite("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for write");
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  serialization::writePod(f, VERSION);
  serialization::writePod(f, count);
  serialization::writeString(f, bookTitle);
  serialization::writeString(f, bookAuthor);
  serialization::writeString(f, bookFilePath);

  for (const auto& bm : bookmarks) {
    serialization::writePod(f, bm.spineIndex);
    serialization::writePod(f, bm.progress);
    serialization::writePod(f, bm.timestamp);
    f.write(reinterpret_cast<const uint8_t*>(bm.chapterTitle), sizeof(bm.chapterTitle));
    serialization::writePod(f, bm.paragraphIndex);
    f.write(reinterpret_cast<const uint8_t*>(bm.snippet), sizeof(bm.snippet));
  }

  f.close();
  LOG_DBG("BKS", "Saved %u bookmark(s)", count);
  return true;
}

void BookmarkStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string currentPath = currentStoreFilePathForBook(filePath, bookType);
  const std::string legacyPath = legacyStoreFilePathForBook(filePath, bookType);
  bool deletedAny = false;

  if (Storage.exists(currentPath.c_str())) {
    deletedAny = deleteBookmarkStorePath(currentPath, "canonical") || deletedAny;
  }
  if (legacyPath != currentPath && Storage.exists(legacyPath.c_str())) {
    deletedAny = deleteBookmarkStorePath(legacyPath, "legacy") || deletedAny;
  }

  if (deletedAny) {
    LOG_DBG("BKS", "Deleted bookmark file for: %s", filePath.c_str());
  }
}

bool BookmarkStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  if (bookType != "epub" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type for bookmark migration: %s", bookType.c_str());
    return false;
  }
  if (oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return true;
  }

  const std::string srcCurrentPath = currentStoreFilePathForBook(oldFilePath, bookType);
  const std::string srcLegacyPath = legacyStoreFilePathForBook(oldFilePath, bookType);
  const std::string dstCurrentPath = currentStoreFilePathForBook(newFilePath, bookType);
  const std::string dstLegacyPath = legacyStoreFilePathForBook(newFilePath, bookType);

  const bool hasSrcCurrent = Storage.exists(srcCurrentPath.c_str());
  const bool hasSrcLegacy = srcLegacyPath != srcCurrentPath && Storage.exists(srcLegacyPath.c_str());
  if (!hasSrcCurrent && !hasSrcLegacy) {
    return true;
  }

  BookmarkStore sourceReader;
  sourceReader.bookFilePath = oldFilePath;
  std::vector<Bookmark> migratedBookmarks;
  bool sourceNeedsRewrite = false;
  bool loadedSourceBookmarks = false;

  if (hasSrcCurrent) {
    bool currentNeedsRewrite = false;
    if (!sourceReader.readFromFile(srcCurrentPath, migratedBookmarks, currentNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load source bookmark file during path migration: %s", srcCurrentPath.c_str());
      return false;
    }
    sourceNeedsRewrite = currentNeedsRewrite;
    loadedSourceBookmarks = true;
  }

  if (hasSrcLegacy) {
    bool legacyNeedsRewrite = false;
    std::vector<Bookmark> legacyBookmarks;
    if (!sourceReader.readFromFile(srcLegacyPath, legacyBookmarks, legacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load source legacy bookmark file during path migration: %s", srcLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(migratedBookmarks, legacyBookmarks);
    sourceNeedsRewrite = sourceNeedsRewrite || legacyNeedsRewrite;
    loadedSourceBookmarks = true;
  }

  if (!loadedSourceBookmarks) {
    return true;
  }

  BookmarkStore destReader;
  destReader.bookFilePath = newFilePath;
  bool hasDestBookmarks = false;

  if (Storage.exists(dstCurrentPath.c_str())) {
    bool dstNeedsRewrite = false;
    std::vector<Bookmark> existingBookmarks;
    if (!destReader.readFromFile(dstCurrentPath, existingBookmarks, dstNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination bookmark file during path migration: %s", dstCurrentPath.c_str());
      return false;
    }
    mergeBookmarks(existingBookmarks, migratedBookmarks);
    migratedBookmarks = std::move(existingBookmarks);
    hasDestBookmarks = true;
  }

  if (dstLegacyPath != dstCurrentPath && Storage.exists(dstLegacyPath.c_str())) {
    bool dstLegacyNeedsRewrite = false;
    std::vector<Bookmark> existingLegacyBookmarks;
    if (!destReader.readFromFile(dstLegacyPath, existingLegacyBookmarks, dstLegacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination legacy bookmark file during path migration: %s",
              dstLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(existingLegacyBookmarks, migratedBookmarks);
    migratedBookmarks = std::move(existingLegacyBookmarks);
    hasDestBookmarks = true;
  }

  BookmarkStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = dstCurrentPath;
  writer.bookmarks = std::move(migratedBookmarks);

  if (!writer.bookmarks.empty()) {
    if (!writer.writeToFile()) {
      LOG_ERR("BKS", "Failed to write migrated bookmark file: %s", dstCurrentPath.c_str());
      return false;
    }
  } else if (Storage.exists(dstCurrentPath.c_str()) && !deleteBookmarkStorePath(dstCurrentPath, "empty migrated")) {
    return false;
  }

  if (!deleteBookmarkStorePath(srcCurrentPath, "source")) {
    return false;
  }
  if (srcLegacyPath != srcCurrentPath && !deleteBookmarkStorePath(srcLegacyPath, "source legacy")) {
    return false;
  }
  if (dstLegacyPath != dstCurrentPath && !deleteBookmarkStorePath(dstLegacyPath, "destination legacy")) {
    return false;
  }

  LOG_INF("BKS", "Migrated bookmark path: %s -> %s (%u bookmark(s)%s%s)", oldFilePath.c_str(), newFilePath.c_str(),
          static_cast<unsigned>(writer.bookmarks.size()), hasDestBookmarks ? ", merged with destination" : "",
          sourceNeedsRewrite ? ", normalized format" : "");
  return true;
}

bool BookmarkStore::hasAnyBookmarks() {
  if (!Storage.exists(BOOKMARKS_DIR)) return false;
  return !Storage.listFiles(BOOKMARKS_DIR).empty();
}

bool BookmarkStore::getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out) {
  if (!Storage.exists(BOOKMARKS_DIR)) return true;

  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  for (const auto& name : files) {
    const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();

    FsFile f;
    if (!Storage.openFileForRead("BKS", fullPath, f)) continue;

    if (f.available() < static_cast<int>(sizeof(uint8_t))) {
      f.close();
      continue;
    }
    uint8_t version;
    serialization::readPod(f, version);
    if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
        version != VERSION) {
      LOG_DBG("BKS", "Skipping bookmark file with unknown version: %s", name.c_str());
      f.close();
      continue;
    }

    if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
      f.close();
      continue;
    }
    uint16_t count = 0;
    if (!readBookmarkCount(f, version, count)) {
      f.close();
      continue;
    }

    // Reads a length-prefixed string, returning false if the file is truncated.
    auto readCheckedString = [&f](std::string& s) -> bool {
      uint32_t len;
      if (f.available() < static_cast<int>(sizeof(len))) return false;
      serialization::readPod(f, len);
      if (f.available() < static_cast<int>(len)) return false;
      s.resize(len);
      f.read(reinterpret_cast<uint8_t*>(&s[0]), len);
      return true;
    };

    std::string title, author, path;
    if (!readCheckedString(title) || !readCheckedString(author) || !readCheckedString(path)) {
      f.close();
      continue;
    }
    f.close();

    std::string bookType = "epub";
    const std::string nameStr = name.c_str();
    size_t underscorePos = nameStr.find('_');
    if (underscorePos != std::string::npos) {
      bookType = nameStr.substr(0, underscorePos);
    }

    if (path.empty() || count == 0) continue;
    if (!Storage.exists(path.c_str())) {
      if (bookType != "epub") continue;

      std::string movedPath;
      if (!resolveMovedToReadDestinationPath(path, movedPath)) {
        continue;
      }
      if (!BookmarkStore::migrateForFilePath(path, movedPath, title, author, bookType)) {
        continue;
      }
      path = std::move(movedPath);
    }

    auto existing = std::find_if(out.begin(), out.end(), [&](const BookmarkedBookEntry& entry) {
      return entry.bookPath == path && entry.bookType == bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, count);
      if (existing->bookTitle.empty()) existing->bookTitle = title;
      if (existing->bookAuthor.empty()) existing->bookAuthor = author;
      continue;
    }

    out.push_back({std::move(title), std::move(author), std::move(path), std::move(bookType), count});
  }

  return true;
}
