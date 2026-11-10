#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  String(size_t value) : std::string(std::to_string(value)) {}
  bool isEmpty() const { return empty(); }
  bool startsWith(const char* text) const { return rfind(text, 0) == 0; }
  bool endsWith(const char* text) const {
    const size_t length = strlen(text);
    return size() >= length && compare(size() - length, length, text) == 0;
  }
  char charAt(size_t i) const { return i < size() ? at(i) : 0; }
  int indexOf(char value, size_t start = 0) const { return index(find(value, start)); }
  int indexOf(const char* value, size_t start = 0) const { return index(find(value, start)); }
  int lastIndexOf(char value) const { return index(rfind(value)); }
  String substring(size_t from, size_t to = npos) const { return substr(from, to == npos ? npos : to - from); }

 private:
  static int index(size_t result) { return result == npos ? -1 : static_cast<int>(result); }
};
template <typename... Args>
void testLog(Args&&...) {}
#define LOG_ERR(...) testLog(__VA_ARGS__)
#define LOG_DBG(...) testLog(__VA_ARGS__)
void yield() {}
struct {
  bool showHiddenFiles = false;
} SETTINGS;
enum HTTPMethod {
  HTTP_OPTIONS,
  HTTP_PROPFIND,
  HTTP_GET,
  HTTP_HEAD,
  HTTP_PUT,
  HTTP_DELETE,
  HTTP_MKCOL,
  HTTP_MOVE,
  HTTP_COPY,
  HTTP_LOCK,
  HTTP_UNLOCK
};
enum { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };
struct HTTPRaw {
  int status;
  const uint8_t* buf = nullptr;
  size_t currentSize = 0, totalSize = 0;
};
struct NetworkClient {
  void stop() {}
};
constexpr size_t CONTENT_LENGTH_UNKNOWN = -1;
struct WebServer {
  HTTPMethod verb = HTTP_GET;
  String path = "/", destination, depth = "1", body;
  int status = 0;
  NetworkClient socket;
  HTTPMethod method() const { return verb; }
  String uri() const { return path; }
  String header(const char* name) const {
    if (strcmp(name, "Destination") == 0) return destination;
    if (strcmp(name, "Depth") == 0) return depth;
    return "";
  }
  void send(int code, const char* = "", const String& content = "") {
    status = code;
    body = content;
  }
  void sendContent(const String& content) { body += content; }
  void sendHeader(const char*, const char*) {}
  void setContentLength(size_t) {}
  size_t clientContentLength() const { return 0; }
  NetworkClient& client() { return socket; }
  static String urlDecode(const String& value) {
    String result;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '%' && i + 2 < value.size()) {
        result += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else
        result += value[i];
    }
    return result;
  }
};
struct RequestHandler {
  virtual bool canHandle(WebServer&, HTTPMethod, const String&) = 0;
  virtual bool canRaw(WebServer&, const String&) = 0;
  virtual void raw(WebServer&, const String&, HTTPRaw&) = 0;
  virtual bool handle(WebServer&, HTTPMethod, const String&) = 0;
  virtual ~RequestHandler() = default;
};
struct Node {
  bool directory;
  std::string data;
};
struct HalFile {
  std::string path;
  bool opened = false;
  size_t cursor = 0;
  explicit operator bool() const { return opened; }
  bool isDirectory() const;
  size_t getName(char* buffer, size_t size) const;
  size_t size() const;
  int available() const;
  int read(void*, size_t);
  size_t write(const void*, size_t);
  bool close() {
    opened = false;
    return true;
  }
  bool sync() { return true; }
  HalFile openNextFile();
};
struct StorageMock {
  std::map<std::string, Node> files;
  std::map<std::string, std::string> aliases;
  std::set<std::string> failOpen, failRename;
  int mutations = 0;
  // Model FAT name spelling and alias resolution independently of the policy.
  std::string canonical(const std::string& path) const {
    std::string result;
    size_t start = 0;
    while (start < path.size()) {
      size_t end = path.find('/', start);
      if (end == std::string::npos) end = path.size();
      std::string name = path.substr(start, end - start);
      start = end + 1;
      while (!name.empty() && name.front() == ' ') name.erase(0, 1);
      while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
      if (name.empty()) continue;
      for (char& c : name)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      result += '/' + name;
      const auto alias = aliases.find(result);
      if (alias != aliases.end()) result = alias->second;
    }
    return result.empty() ? "/" : result;
  }
  bool exists(const char* path) const { return files.count(canonical(path)); }
  HalFile open(const char* path) {
    const auto key = canonical(path);
    return {key, files.count(key) != 0 && !failOpen.count(key), 0};
  }
  bool remove(const char* path) {
    ++mutations;
    return files.erase(canonical(path));
  }
  bool rmdir(const char* path) { return remove(path); }
  bool mkdir(const char* path) {
    ++mutations;
    files[canonical(path)] = {true, ""};
    return true;
  }
  bool rename(const char* from, const char* to) {
    ++mutations;
    const auto src = canonical(from), dst = canonical(to);
    if (failRename.count(src) || !files.count(src) || files.count(dst)) return false;
    files[dst] = files.at(src);
    files.erase(src);
    return true;
  }
  bool openFileForWrite(const char*, const String& path, HalFile& file) {
    ++mutations;
    files[canonical(path)] = {false, ""};
    file = open(path.c_str());
    return bool(file);
  }
  bool readFileToStream(const char* path, NetworkClient&) { return exists(path); }
} Storage;
bool HalFile::isDirectory() const { return Storage.files.at(path).directory; }
size_t HalFile::getName(char* buffer, size_t length) const {
  const auto name = path.substr(path.rfind('/') + 1);
  return snprintf(buffer, length, "%s", name.c_str());
}
size_t HalFile::size() const { return Storage.files.at(path).data.size(); }
int HalFile::available() const { return size() - cursor; }
int HalFile::read(void* buffer, size_t length) {
  length = std::min(length, size() - cursor);
  memcpy(buffer, Storage.files.at(path).data.data() + cursor, length);
  cursor += length;
  return length;
}
size_t HalFile::write(const void* buffer, size_t length) {
  Storage.files.at(path).data.append(static_cast<const char*>(buffer), length);
  return length;
}
HalFile HalFile::openNextFile() {
  std::vector<std::string> children;
  const auto prefix = path == "/" ? path : path + '/';
  for (const auto& entry : Storage.files) {
    if (entry.first != path && entry.first.rfind(prefix, 0) == 0 &&
        entry.first.find('/', prefix.size()) == std::string::npos)
      children.push_back(entry.first);
  }
  if (cursor >= children.size()) return {};
  return Storage.open(children[cursor++].c_str());
}
namespace FsHelpers {
bool directoryCanBeEnumerated(const char*) { return true; }
bool directoryIterationFailed(const HalFile&) { return false; }
enum class DirectoryEntryVisibility { Visible, Missing };
DirectoryEntryVisibility directoryEntryVisibility(const char*, const char*) {
  return DirectoryEntryVisibility::Visible;
}
bool checkFileExtension(const String& path, const char* ext) { return path.endsWith(ext); }
bool hasEpubExtension(const String& path) { return path.endsWith(".epub"); }
bool hasTxtExtension(const String& path) { return path.endsWith(".txt"); }
bool hasJpgExtension(const String& path) { return path.endsWith(".jpg"); }
bool hasPngExtension(const String& path) { return path.endsWith(".png"); }
bool hasGifExtension(const String& path) { return path.endsWith(".gif"); }
}  // namespace FsHelpers
struct {
  void markRegistryDirtyForPath(const char*) {}
} sdFontSystem;
struct ImageFolderIndex {
  static void invalidateForPath(const char*) {}
};
bool clearBookCachePreservingUserState(const char*, bool) { return true; }
void clearBookCache(const char*) {}
namespace bookkey {
std::set<std::string> sidecars;
int moves = 0;
bool hasSidecar(const char* path) { return sidecars.count(Storage.canonical(path)); }
bool moveFile(const char* from, const char* to) {
  ++moves;
  return Storage.rename(from, to);
}
}  // namespace bookkey
#include "Production.inc"

void reset() {
  Storage = {};
  bookkey::sidecars.clear();
  bookkey::moves = 0;
  Storage.files = {{"/", {true, ""}},
                   {"/.crosspoint", {true, ""}},
                   {"/.crosspoint/wifi.json", {false, "secret"}},
                   {"/.crosspoint/wifi.json.bak", {false, "old-secret"}},
                   {"/.crosspoint/recent.json", {false, "cache"}},
                   {"/.hidden", {false, "user-hidden"}},
                   {"/books", {true, ""}},
                   {"/books/book.epub", {false, "book"}},
                   {"/img~1.jpg", {false, "photo"}},
                   {"/crossp~9", {false, "literal-name"}}};
  Storage.aliases = {{"/crossp~1", "/.crosspoint"},
                     {"/cro~a123", "/.crosspoint"},
                     {"/.crosspoint/wifi~1.jso", "/.crosspoint/wifi.json"},
                     {"/.crosspoint/wifi~2.bak", "/.crosspoint/wifi.json.bak"}};
}
WebServer request(WebDAVHandler& handler, HTTPMethod method, const char* path, const char* destination = "") {
  WebServer server;
  server.verb = method;
  server.path = path;
  server.destination = destination;
  assert(handler.handle(server, method, server.path));
  return server;
}
int main() {
  size_t denied = 0;
  for (bool hidden : {false, true}) {
    SETTINGS.showHiddenFiles = hidden;
    for (const char* path : {"/.crosspoint/%20..%20", "/CROSSP~1/", "/CROSSP~1", "/CRO~A123", "/.crosspoint/wifi.json",
                             "/CROSSP~1/WIFI~1.JSO", "/%20.CrossPoint.%20/%20WiFi.JsOn.%20", "/.crosspoint/WIFI~2.BAK",
                             "/.crosspoint/opds.json", "/.crosspoint/koreader.json.tmp", "/.crosspoint/wifi.bin.bak",
                             "/.crosspoint/settings.json", "/.crosspoint/settings.bin.bak",
                             "/.crosspoint/crossink-settings.json.bak", "/.crosspoint/plugin-install.txt.part"}) {
      for (HTTPMethod method :
           {HTTP_GET, HTTP_HEAD, HTTP_PROPFIND, HTTP_PUT, HTTP_DELETE, HTTP_MKCOL, HTTP_MOVE, HTTP_COPY}) {
        reset();
        WebDAVHandler handler;
        const auto result = request(handler, method, path, "/books/out");
        assert(result.status == 403);
        assert(Storage.mutations == 0);
        ++denied;
      }
      for (HTTPMethod method : {HTTP_MOVE, HTTP_COPY}) {
        reset();
        WebDAVHandler handler;
        assert(request(handler, method, "/books/book.epub", path).status == 403);
        assert(Storage.mutations == 0);
        ++denied;
      }
      reset();
      WebDAVHandler handler;
      WebServer server;
      server.verb = HTTP_PUT;
      server.path = path;
      for (int status : {RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED}) {
        HTTPRaw raw{status};
        handler.raw(server, server.path, raw);
      }
      assert(Storage.mutations == 0);
    }
    reset();
    WebDAVHandler handler;
    assert(request(handler, HTTP_GET, "/img~1.jpg").status == 403);
    assert(request(handler, HTTP_GET, "/crossp~9").status == 403);
    assert(request(handler, HTTP_GET, "/.hidden").status == (hidden ? 200 : 403));
    assert(request(handler, HTTP_GET, "/%20.hidden.%20").status == (hidden ? 200 : 403));
    assert(request(handler, HTTP_GET, "/%20system%20volume%20information.%20/file").status == 403);
    assert(request(handler, HTTP_GET, "/%20xtcache.%20/file").status == 403);
    assert(request(handler, HTTP_GET, "/.crosspoint/recent.json").status == (hidden ? 200 : 403));
    auto listing = request(handler, HTTP_PROPFIND, "/");
    assert(listing.status == 207);
    assert((listing.body.find(".crosspoint") != std::string::npos) == hidden);
    assert(listing.body.find("img~1.jpg") == std::string::npos);
    assert((listing.body.find(".hidden") != std::string::npos) == hidden);
  }
  SETTINGS.showHiddenFiles = true;
  reset();
  WebDAVHandler handler;
  // Preserve the local shared policy: aliases at credential depth are denied.
  // Full long Unicode names remain directly accessible.
  std::string unicodeName = "/";
  for (int i = 0; i < 250; ++i) unicodeName += "書";
  Storage.files[unicodeName] = {false, "unicode"};
  Storage.aliases["/book~1.txt"] = unicodeName;
  assert(request(handler, HTTP_GET, "/BOOK~1.TXT").status == 403);
  assert(request(handler, HTTP_GET, unicodeName.c_str()).status == 200);
  Storage.failOpen.insert("/.crosspoint");
  assert(request(handler, HTTP_GET, "/CROSSP~1/wifi.json").status == 403);
  reset();
  assert(request(handler, HTTP_COPY, "/books/book.epub", "http://reader/%20.crosspoint/wifi.json").status == 403);
  assert(Storage.mutations == 0);
  // Browsing the credential container remains safe; only stores are omitted.
  auto listing = request(handler, HTTP_PROPFIND, "/.crosspoint");
  assert(listing.status == 207);
  assert(listing.body.find("wifi.json") == std::string::npos);
  assert(listing.body.find("recent.json") != std::string::npos);
  for (const char* path : {"/.crosspoint", "/.crosspoint/.", "/%20.CrossPoint.%20"}) {
    for (HTTPMethod method : {HTTP_DELETE, HTTP_MOVE, HTTP_COPY}) {
      assert(request(handler, method, path, "/books/out").status == 403);
      assert(Storage.mutations == 0);
    }
  }
  // Existing device-bound book movement must still use the SDK sidecar helper.
  bookkey::sidecars.insert("/books/book.epub");
  Storage.files["/books/target.epub"] = {false, "original"};
  assert(request(handler, HTTP_MOVE, "/books/book.epub", "/books/target.epub").status == 409);
  assert(Storage.mutations == 0 && bookkey::moves == 0);
  assert(request(handler, HTTP_MOVE, "/books/book.epub", "/books/new.epub").status == 201);
  assert(bookkey::moves == 1);
  reset();
  // Existing replacement rollback stays intact.
  Storage.files["/books/target.epub"] = {false, "original"};
  Storage.failRename.insert("/books/book.epub");
  assert(request(handler, HTTP_MOVE, "/books/book.epub", "/books/target.epub").status == 500);
  assert(Storage.files.at("/books/target.epub").data == "original");
  Storage.failRename.clear();
  assert(request(handler, HTTP_MOVE, "/books/book.epub", "/books/target.epub").status == 204);
  assert(Storage.files.at("/books/target.epub").data == "book");
  assert(request(handler, HTTP_COPY, "/books/target.epub", "/books/copy.epub").status == 201);
  assert(request(handler, HTTP_DELETE, "/books/copy.epub").status == 204);
  // Ordinary tilde names remain valid PUT destinations.
  WebServer put;
  put.verb = HTTP_PUT;
  put.path = "/books/img~photo.jpg";
  const uint8_t data[] = {'o', 'k'};
  HTTPRaw raw{RAW_START};
  handler.raw(put, put.path, raw);
  raw = {RAW_WRITE, data, sizeof(data), sizeof(data)};
  handler.raw(put, put.path, raw);
  raw.status = RAW_END;
  handler.raw(put, put.path, raw);
  assert(request(handler, HTTP_PUT, put.path.c_str()).status == 201);
  assert(Storage.files.at("/books/img~photo.jpg").data == "ok");
  printf(
      "PASS: %zu protected method/destination cases, raw PUT, FAT aliases, listings, hidden policy, cache access and "
      "transfers/rollback\n",
      denied);
}
