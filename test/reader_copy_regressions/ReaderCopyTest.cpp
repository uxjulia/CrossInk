#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
struct String : std::string {
  using std::string::string;
  String(std::string s) : std::string(std::move(s)) {}
  bool isEmpty() const { return empty(); }
  int lastIndexOf(char c) const {
    auto p = rfind(c);
    return p == npos ? -1 : static_cast<int>(p);
  }
  String substring(int begin, int end) const { return substr(begin, end - begin); }
};
struct Io {
  std::map<std::string, std::string> files;
  int failAt = -1, zeroAt = -1;
  bool shortWrite = false;
  int openHandles = 0, cacheClears = 0;
} io;
struct HalFile {
  std::string path;
  size_t pos = 0;
  bool opened = false;
  explicit operator bool() const { return opened; }
  bool isDirectory() const { return false; }
  size_t size() const { return io.files.at(path).size(); }
  bool available() const { return pos < size(); }
  int read(uint8_t* out, size_t count) {
    if (io.failAt >= 0 && pos >= static_cast<size_t>(io.failAt)) return -1;
    if (io.zeroAt >= 0 && pos >= static_cast<size_t>(io.zeroAt)) return 0;
    count = std::min(count, io.files.at(path).size() - pos);
    if (io.failAt >= 0) count = std::min(count, static_cast<size_t>(io.failAt) - pos);
    std::memcpy(out, io.files.at(path).data() + pos, count);
    pos += count;
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* bytes, size_t count) {
    if (io.shortWrite && count) --count;
    io.files[path].append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
  bool sync() const { return opened; }
  bool close() {
    if (opened) --io.openHandles;
    opened = false;
    return true;
  }
};
struct StorageMock {
  bool exists(const char* path) const { return io.files.contains(path); }
  bool remove(const char* path) { return io.files.erase(path) > 0; }
  HalFile open(const char* path) {
    if (!exists(path)) return {};
    ++io.openHandles;
    return {path, 0, true};
  }
  bool openFileForWrite(const char*, const String& path, HalFile& file) {
    io.files[path] = "";
    ++io.openHandles;
    file = {path, 0, true};
    return true;
  }
} Storage;
namespace FilePublication {
constexpr char copyStage[] = "/.crosspoint/xfer/dav-copy";
bool recover() { return true; }
bool stagePath(const char*, const char*, std::string& path) {
  path = copyStage;
  return true;
}
bool publish(const char* staged, const char* destination, bool replace, bool) {
  assert(io.openHandles == 0);
  assert(replace || !Storage.exists(destination));
  io.files[destination] = std::move(io.files.at(staged));
  return Storage.remove(staged);
}
}  // namespace FilePublication
struct WebServer {
  int code = 0;
  void send(int c, const char* = nullptr, const char* = nullptr) { code = c; }
};
struct WebDAVHandler {
  String source = "/src", destination = "/dst";
  bool overwrite = true;
  String getRequestPath(WebServer&) { return source; }
  String getDestinationPath(WebServer&) { return destination; }
  bool getOverwrite(WebServer&) { return overwrite; }
  bool isProtectedPath(const String&) { return false; }
  void handleCopy(WebServer&);
};
namespace protectedpaths {
bool isSensitivePath(const char*, bool) { return false; }
}  // namespace protectedpaths
struct Fonts {
  void markRegistryDirtyForPath(const char*) {}
} sdFontSystem;
namespace ImageFolderIndex {
void invalidateForPath(const char*) {}
}  // namespace ImageFolderIndex
bool clearBookCachePreservingUserState(const char*, bool) {
  ++io.cacheClears;
  return true;
}

// URL normalization is outside this regression; the production resolver consumes
// already decoded, normalized paths in these cases.
namespace FsHelpers {
bool writeAndCheckpoint(HalFile& file, const uint8_t* data, size_t length) {
  const auto written = file.write(data, length);
  return file.sync() && written == length;
}
std::string normalisePath(const std::string& s) { return s; }
std::string decodeUriEscapes(const std::string& s) { return s; }
}  // namespace FsHelpers
struct Metadata {
  bool loaded = true;
  bool isLoaded() const { return loaded; }
} metadata;
struct Spine {
  std::string href;
};
struct Epub {
  Metadata* bookMetadataCache = &metadata;
  std::vector<Spine> spine;
  int guide = 2, guideReads = 0;
  int getSpineItemsCount() const { return spine.size(); }
  const Spine& getSpineItem(int i) const { return spine.at(i); }
  int resolveHrefToSpineIndex(const std::string&) const;
  int getSpineIndexForTextReference() {
    ++guideReads;
    return guide;
  }
};
namespace EpubReaderUtils {
struct Progress {
  int spineIndex = 0, pageNumber = 3, pageCount = 10;
  bool hasPageCount = true, hasVisibleTextOffset = false;
  unsigned visibleTextOffset = 12;
} saved;
bool valid = true;
bool loadProgress(Epub&, Progress& progress) {
  progress = saved;
  return valid;
}
}  // namespace EpubReaderUtils
struct AppState {
  uint16_t pendingBookmarkSpine = UINT16_MAX;
  float pendingBookmarkProgress = -1;
  uint16_t pendingBookmarkParagraphIndex = UINT16_MAX;
  uint16_t pendingClippingIndex = UINT16_MAX;
  void saveToFile() {}
} APP_STATE;
struct Reader {
  std::unique_ptr<Epub> epub = std::make_unique<Epub>();
  int currentSpineIndex = 0, nextPageNumber = 0, cachedSpineIndex = -1;
  float pendingSpineProgress = -1;
  uint16_t pendingParagraphIndex = UINT16_MAX, pendingClippingIndex = UINT16_MAX;
  bool pendingPercentJump = false, pendingRelayoutReposition = false;
  int cachedChapterPageNumber = -1, cachedChapterTotalPageCount = -1;
  unsigned cachedVisibleTextOffset = 0;
  void restore();
};
#include "Production.inc"

int main() {
  for (bool overwrite : {false, true}) {
    for (int failure : {-1, 0, 4096}) {
      io = {};
      io.files["/src"] = std::string(9000, 'x');
      if (overwrite) io.files["/dst"] = "previous destination";
      io.failAt = failure;
      WebDAVHandler handler;
      WebServer server;
      handler.handleCopy(server);
      assert(io.openHandles == 0);
      assert(!Storage.exists(FilePublication::copyStage));
      assert(io.files.at("/src") == std::string(9000, 'x'));
      if (failure < 0) {
        assert(server.code == (overwrite ? 204 : 201));
        assert(io.files.at("/dst") == io.files.at("/src"));
        assert(io.cacheClears == 1);
      } else {
        assert(server.code == 500 && io.cacheClears == 0);
        if (overwrite) {
          assert(io.files.at("/dst") == "previous destination");
        } else {
          assert(!io.files.contains("/dst"));
        }
      }
    }
  }
  io = {};
  io.files["/src"] = "";
  WebDAVHandler handler;
  WebServer server;
  handler.handleCopy(server);
  assert(server.code == 201 && io.files.at("/dst").empty() && io.openHandles == 0);
  io = {};
  io.files = {{"/src", "new"}, {"/dst", "old"}};
  handler.overwrite = false;
  handler.handleCopy(server);
  assert(server.code == 412 && io.files.at("/dst") == "old" && io.openHandles == 0);
  handler.overwrite = true;
  io = {};
  io.files["/src"] = "bytes";
  io.shortWrite = true;
  handler.handleCopy(server);
  assert(server.code == 500 && !io.files.contains("/dst") && io.openHandles == 0);

  Epub book;
  book.spine = {{"a/chapter.xhtml"}, {"b/chapter.xhtml"}, {"other.xhtml"}};
  assert(book.resolveHrefToSpineIndex("b/chapter.xhtml#note") == 1);
  assert(book.resolveHrefToSpineIndex("a/chapter.xhtml") == 0);
  assert(book.resolveHrefToSpineIndex("c/chapter.xhtml") == 0);
  assert(book.resolveHrefToSpineIndex("other.xhtml") == 2);
  assert(book.resolveHrefToSpineIndex("missing.xhtml") == -1);
  assert(book.resolveHrefToSpineIndex("#note") == -1);
  metadata.loaded = false;
  assert(book.resolveHrefToSpineIndex("other.xhtml") == -1);
  metadata.loaded = true;

  for (int spine : {0, 1}) {
    for (bool offset : {false, true}) {
      EpubReaderUtils::saved = {};
      EpubReaderUtils::saved.spineIndex = spine;
      EpubReaderUtils::saved.hasVisibleTextOffset = offset;
      EpubReaderUtils::valid = true;
      APP_STATE = {};
      Reader reader;
      reader.restore();
      assert(reader.currentSpineIndex == spine && reader.nextPageNumber == 3);
      assert(reader.cachedSpineIndex == spine && reader.epub->guideReads == 0);
      assert(reader.pendingRelayoutReposition == offset);
      assert(!offset || reader.cachedVisibleTextOffset == 12);
    }
  }
  EpubReaderUtils::valid = false;
  Reader fresh;
  fresh.restore();
  assert(fresh.currentSpineIndex == 2 && fresh.epub->guideReads == 1);
  Reader noGuide;
  noGuide.epub->guide = 0;
  noGuide.restore();
  assert(noGuide.currentSpineIndex == 0);
  APP_STATE.pendingBookmarkSpine = 0;
  APP_STATE.pendingBookmarkProgress = 0.3f;
  Reader bookmark;
  bookmark.restore();
  assert(bookmark.currentSpineIndex == 0 && bookmark.pendingPercentJump && bookmark.epub->guideReads == 0);
  assert(APP_STATE.pendingBookmarkSpine == UINT16_MAX);
}
