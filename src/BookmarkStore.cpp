#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <functional>

namespace {
constexpr uint8_t VERSION = 2;
constexpr uint8_t MAX_BOOKMARKS = 32;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";
}  // namespace

BookmarkStore BookmarkStore::instance;

bool BookmarkStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  bookmarks.clear();
  bookmarks.reserve(MAX_BOOKMARKS);

  const size_t hash = std::hash<std::string>{}(filePath);
  storeFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(hash) + ".bin";

  if (!Storage.exists(storeFilePath.c_str())) {
    LOG_DBG("BKS", "No bookmark file for this book");
    return true;
  }

  return readFromFile();
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

void BookmarkStore::addBookmark(uint16_t spineIndex, float progress, const char* chapterTitle) {
  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%d) reached", MAX_BOOKMARKS);
    return;
  }

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.timestamp = 0;  // ESP32-C3 has no battery-backed RTC; reserved for future use
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");

  bookmarks.push_back(bm);
  dirty = true;
  saveToFile();
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
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

bool BookmarkStore::hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  for (const auto& b : bookmarks) {
    if (b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd) return true;
  }
  return false;
}

void BookmarkStore::saveToFile() {
  if (!dirty || storeFilePath.empty()) return;
  if (writeToFile()) dirty = false;
}

void BookmarkStore::clearAll() {
  bookmarks.clear();
  dirty = false;
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    if (!Storage.remove(storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to delete bookmark file");
      return;
    }
    LOG_DBG("BKS", "Bookmark file deleted");
  }
}

bool BookmarkStore::readFromFile() {
  FsFile f;
  if (!Storage.openFileForRead("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for read");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != VERSION) {
    LOG_ERR("BKS", "Unknown bookmark file version: %u", version);
    f.close();
    return false;
  }

  uint8_t count;
  serialization::readPod(f, count);
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark count %u exceeds max, file may be corrupt", count);
    f.close();
    return false;
  }

  // Skip stored header strings — we already have title/author/path in memory
  std::string tmp;
  serialization::readString(f, tmp);
  serialization::readString(f, tmp);
  serialization::readString(f, tmp);

  bookmarks.clear();
  bookmarks.reserve(count);
  for (uint8_t i = 0; i < count; i++) {
    Bookmark bm{};
    serialization::readPod(f, bm.spineIndex);
    serialization::readPod(f, bm.progress);
    serialization::readPod(f, bm.timestamp);
    f.read(bm.chapterTitle, sizeof(bm.chapterTitle));
    bookmarks.push_back(bm);
  }

  f.close();
  LOG_DBG("BKS", "Loaded %u bookmark(s)", count);
  return true;
}

bool BookmarkStore::writeToFile() const {
  Storage.mkdir(BOOKMARKS_DIR);

  FsFile f;
  if (!Storage.openFileForWrite("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for write");
    return false;
  }

  const uint8_t count = static_cast<uint8_t>(bookmarks.size());
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
  }

  f.close();
  LOG_DBG("BKS", "Saved %u bookmark(s)", count);
  return true;
}

bool BookmarkStore::getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out) {
  if (!Storage.exists(BOOKMARKS_DIR)) return true;

  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  for (const auto& name : files) {
    const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();

    FsFile f;
    if (!Storage.openFileForRead("BKS", fullPath, f)) continue;

    uint8_t version;
    serialization::readPod(f, version);
    if (version != VERSION) {
      LOG_DBG("BKS", "Skipping bookmark file with unknown version: %s", name.c_str());
      f.close();
      continue;
    }

    uint8_t count;
    serialization::readPod(f, count);

    std::string title, author, path;
    serialization::readString(f, title);
    serialization::readString(f, author);
    serialization::readString(f, path);
    f.close();

    if (path.empty() || count == 0) continue;

    out.push_back({std::move(title), std::move(author), std::move(path), count});
  }

  return true;
}
