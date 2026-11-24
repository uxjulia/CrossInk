// Handler bodies are extracted from production; network and SD I/O are deterministic fakes.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../lib/EpdFont/FontFileTypes.h"

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CROSSINK_SCALABLE_FONTS 1

class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
  bool isEmpty() const { return empty(); }
  bool endsWith(const char* suffix) const {
    return size() >= strlen(suffix) && compare(size() - strlen(suffix), strlen(suffix), suffix) == 0;
  }
};
struct HalFile {
  bool opened = true;
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  explicit operator bool() const { return opened; }
  bool isOpen() const { return opened; }
  bool failWrite = false;
  bool failSync = false;
  bool failClose = false;
  bool close() {
    opened = false;
    return !failClose;
  }
  bool sync() const { return !failSync; }
  uint64_t fileSize64() const { return bytes.size(); }
  uint64_t position() const { return cursor; }
  size_t write(const uint8_t* data, size_t size) {
    assert(opened);
    if (failWrite) return 0;
    bytes.insert(bytes.end(), data, data + size);
    cursor += size;
    return size;
  }
};
struct StorageMock {
  std::vector<std::string> removed;
  bool remove(const char* path) {
    removed.emplace_back(path);
    return true;
  }
} Storage;
std::vector<std::string> published;
namespace FilePublication {
constexpr char DIRECTORY[] = "/.crosspoint/xfer";
constexpr char HTTP_STAGE[] = "/.crosspoint/xfer/http";
constexpr char WS_STAGE[] = "/.crosspoint/xfer/ws";
bool prepare() { return true; }
bool publish(const char*, const char* destination, bool = true, bool = true) {
  published.emplace_back(destination);
  return true;
}
}  // namespace FilePublication
constexpr const auto& HTTP_STAGE = FilePublication::HTTP_STAGE;
namespace webtransfer {
bool isWebSocketUploadInProgress() { return false; }
}  // namespace webtransfer
struct FontSystem {
  void markRegistryDirtyForPath(const char*) {}
} sdFontSystem;
struct ImageFolderIndex {
  static void invalidateForPath(const char*) {}
};
unsigned cacheClears = 0;
bool cacheRefreshSucceeds = true;
bool clearBookCachePreservingUserState(const char*, bool sourceChanged) {
  assert(sourceChanged);
  ++cacheClears;
  return cacheRefreshSucceeds;
}
unsigned long millis() { return 100; }
unsigned long uploadStartTime = 0;
struct FontInstaller {
  static unsigned validations;
  static inline bool accepted = true;
  static bool finishUpload(const char* path, bool writesOk);
  static bool validateCpfontFile(const char*) {
    ++validations;
    return accepted;
  }
};
unsigned FontInstaller::validations = 0;
enum { UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status = UPLOAD_FILE_WRITE;
  const uint8_t* buf = nullptr;
  size_t currentSize = 0;
  size_t totalSize = 0;
};
struct Client {
  bool connected = true;
  void stop() { connected = false; }
};
struct Server {
  HTTPUpload body;
  Client socket;
  HTTPUpload& upload() { return body; }
  Client& client() { return socket; }
};
class CrossPointWebServer {
 public:
  struct UploadState {
    HalFile file;
    String path = "/books";
    String fileName = "test.epub";
    String error;
    bool success = false;
    size_t size = 0;
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(UPLOAD_BUFFER_SIZE);
    size_t bufferPos = 0;
  } upload;
  struct FontUploadState {
    HalFile file;
    std::string filePath = "/fonts/Test/Regular.cpfont";
    std::string stagePath = "/.crosspoint/xfer/font.cpfont";
    bool valid = true;
    int errorStatus = 0;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(BUFFER_SIZE);
    size_t bufferPos = 0;
  } fontUpload;
  using UploadCancelCheck = bool (*)(void*);
  UploadCancelCheck uploadCancelCheck = nullptr;
  void* uploadCancelContext = nullptr;
  std::unique_ptr<Server> server = std::make_unique<Server>();
  bool dropUploadIfCancelled() const;
  void abortUpload(UploadState&, const char* error = "Upload aborted") const;
  void abortFontUpload();
  void handleUpload(UploadState&) const;
  void handleFontUploadData();
};
struct MappedInputManager {
  enum class Button { Back };
  unsigned polls = 0;
  bool held = false;
  void update() { ++polls; }
  bool isPressed(Button) { return held; }
};
struct CrossPointWebServerActivity {
  MappedInputManager mappedInput;
  bool leaveRequested = false;
  bool exitEvent = false;
  bool exitRequested() const { return exitEvent; }
  bool checkUploadCancellation();
};
#include "UploadHandlers.inc"

void send(CrossPointWebServer& server, bool font, int status, size_t bytes = 0, size_t totalSize = 0) {
  static uint8_t body[8192] = {};
  assert(bytes <= sizeof(body));
  server.server->body = {status, body, bytes, totalSize ? totalSize : bytes};
  if (font)
    server.handleFontUploadData();
  else
    server.handleUpload(server.upload);
}

int main() {
  for (bool font : {false, true}) {
    // Normal multi-chunk uploads still finish with every byte and no deletion.
    Storage.removed.clear();
    published.clear();
    CrossPointWebServer server;
    send(server, font, UPLOAD_FILE_WRITE, 6000);
    send(server, font, UPLOAD_FILE_WRITE, 77);
    send(server, font, UPLOAD_FILE_END, 0, 6077);
    assert(Storage.removed.empty());
    assert(published == std::vector<std::string>{font ? "/fonts/Test/Regular.cpfont" : "/books/test.epub"});
    assert(font ? server.fontUpload.valid : server.upload.success);
    assert((font ? server.fontUpload.file.bytes.size() : server.upload.file.bytes.size()) == 6077);
    assert(!(font ? server.fontUpload.file.isOpen() : server.upload.file.isOpen()));

    for (int cancelStatus : {UPLOAD_FILE_WRITE, UPLOAD_FILE_END}) {
      Storage.removed.clear();
      published.clear();
      cacheClears = FontInstaller::validations = 0;
      CrossPointWebServer cancelled;
      CrossPointWebServerActivity activity;
      cancelled.uploadCancelCheck = [](void* ctx) {
        return static_cast<CrossPointWebServerActivity*>(ctx)->checkUploadCancellation();
      };
      cancelled.uploadCancelContext = &activity;
      send(cancelled, font, UPLOAD_FILE_WRITE, 5000);  // disk data plus unflushed tail
      activity.mappedInput.held = true;
      send(cancelled, font, cancelStatus, 10, 5010);
      assert(activity.leaveRequested && !cancelled.server->socket.connected);
      assert(!(font ? cancelled.fontUpload.file.isOpen() : cancelled.upload.file.isOpen()));
      assert((font ? cancelled.fontUpload.bufferPos : cancelled.upload.bufferPos) == 0);
      assert(!(font ? cancelled.fontUpload.valid : cancelled.upload.success));
      assert(Storage.removed.front() == (font ? cancelled.fontUpload.stagePath : std::string(HTTP_STAGE)));
      assert(published.empty());
      const auto pollCount = activity.mappedInput.polls;
      activity.mappedInput.held = false;
      // Arduino may still dispatch END or ABORTED after the cancellation callback.
      send(cancelled, font, UPLOAD_FILE_END);
      send(cancelled, font, UPLOAD_FILE_ABORTED);
      assert(activity.mappedInput.polls == pollCount);  // latched cancellation does not poll again
      assert(!(font ? cancelled.fontUpload.valid : cancelled.upload.success));
      assert(cacheClears == 0 && FontInstaller::validations == 0);
    }
    // Unrequested transport abort still cleans up.
    CrossPointWebServer aborted;
    send(aborted, font, UPLOAD_FILE_WRITE, 80);
    send(aborted, font, UPLOAD_FILE_ABORTED);
    assert(!(font ? aborted.fontUpload.file.isOpen() : aborted.upload.file.isOpen()));
  }
  // Mid-body, final-buffer, sync and close failures all leave a retryable path.
  for (int failure = 0; failure < 4; ++failure) {
    Storage.removed.clear();
    published.clear();
    cacheClears = 0;
    CrossPointWebServer failed;
    if (failure == 0) failed.upload.file.failWrite = true;
    send(failed, false, UPLOAD_FILE_WRITE, failure == 0 ? 5000 : 80);
    if (failure == 1) failed.upload.file.failWrite = true;
    if (failure == 2) failed.upload.file.failSync = true;
    if (failure == 3) failed.upload.file.failClose = true;
    send(failed, false, UPLOAD_FILE_END, 0, failure == 0 ? 5000 : 80);
    assert(!failed.upload.success && !failed.upload.error.isEmpty());
    assert(!failed.upload.file.isOpen());
    assert(Storage.removed == std::vector<std::string>{HTTP_STAGE});
    assert(published.empty());
    assert(cacheClears == 0);
    send(failed, false, UPLOAD_FILE_END);
    send(failed, false, UPLOAD_FILE_ABORTED);
    assert(Storage.removed.size() == 1);
  }
  // A completed upload with failed cache/sync preservation reports failure,
  // but keeps the saved file and the preserved reading data for recovery.
  Storage.removed.clear();
  published.clear();
  cacheRefreshSucceeds = false;
  CrossPointWebServer cacheFailed;
  send(cacheFailed, false, UPLOAD_FILE_WRITE, 80);
  send(cacheFailed, false, UPLOAD_FILE_END, 0, 80);
  assert(!cacheFailed.upload.success && !cacheFailed.upload.error.isEmpty());
  assert(!cacheFailed.upload.file.isOpen() && Storage.removed.empty());
  assert(published == std::vector<std::string>{"/books/test.epub"});
  cacheRefreshSucceeds = true;

  Storage.removed.clear();
  published.clear();
  CrossPointWebServer incomplete;
  send(incomplete, false, UPLOAD_FILE_WRITE, 80, 81);
  send(incomplete, false, UPLOAD_FILE_END, 0, 81);
  assert(!incomplete.upload.success && !incomplete.upload.file.isOpen());
  assert(Storage.removed == std::vector<std::string>{HTTP_STAGE});
  assert(published.empty());

  // Exercise production font WRITE/END handlers, not only validation cleanup.
  for (const char* extension : {".ttf", ".OTF", ".cpfont"}) {
    for (int failure = 0; failure < 5; ++failure) {
      Storage.removed.clear();
      published.clear();
      FontInstaller::validations = 0;
      FontInstaller::accepted = failure != 4;
      CrossPointWebServer failed;
      failed.fontUpload.filePath = std::string("/fonts/Test/Regular") + extension;
      failed.fontUpload.stagePath = std::string(FilePublication::DIRECTORY) + "/font" + extension;
      if (failure == 0) failed.fontUpload.file.failWrite = true;
      send(failed, true, UPLOAD_FILE_WRITE, failure == 0 ? 5000 : 80);
      if (failure == 1) failed.fontUpload.file.failWrite = true;
      if (failure == 2) failed.fontUpload.file.failSync = true;
      if (failure == 3) failed.fontUpload.file.failClose = true;
      send(failed, true, UPLOAD_FILE_END, 0, failure == 0 ? 5000 : 80);
      assert(!failed.fontUpload.valid && !failed.fontUpload.file.isOpen());
      assert(!Storage.removed.empty());
      assert(Storage.removed.front() == failed.fontUpload.stagePath);
      assert(published.empty());
      assert(FontInstaller::validations == (failure == 4 ? 1u : 0u));
    }
  }
  FontInstaller::accepted = true;
  Storage.removed.clear();
  published.clear();
  CrossPointWebServer incompleteFont;
  send(incompleteFont, true, UPLOAD_FILE_WRITE, 80, 81);
  send(incompleteFont, true, UPLOAD_FILE_END, 0, 81);
  assert(!incompleteFont.fontUpload.valid && !incompleteFont.fontUpload.file.isOpen());
  assert(Storage.removed == std::vector<std::string>{incompleteFont.fontUpload.stagePath});
  assert(published.empty());
  for (const char* extension : {".ttf", ".OtF"}) {
    Storage.removed.clear();
    published.clear();
    CrossPointWebServer oversized;
    oversized.fontUpload.filePath = std::string("/fonts/Test/Regular") + extension;
    oversized.fontUpload.stagePath = std::string(FilePublication::DIRECTORY) + "/font" + extension;
    oversized.fontUpload.bytesWritten = 2 * 1024 * 1024;
    send(oversized, true, UPLOAD_FILE_WRITE, 1, 1);
    send(oversized, true, UPLOAD_FILE_END, 0, 1);
    assert(!oversized.fontUpload.valid && !oversized.fontUpload.file.isOpen());
    assert(Storage.removed.front() == oversized.fontUpload.stagePath);
    assert(published.empty());
  }
  // A refused collision owns no file and must never delete the existing book.
  Storage.removed.clear();
  CrossPointWebServer collision;
  collision.upload.file.opened = false;
  collision.upload.error = "File already exists";
  send(collision, false, UPLOAD_FILE_END);
  send(collision, false, UPLOAD_FILE_ABORTED);
  assert(Storage.removed.empty());

  // Touch-header/Home/back-release exit events use the same latch as held Back.
  CrossPointWebServerActivity touched;
  touched.exitEvent = true;
  assert(touched.checkUploadCancellation());
  touched.exitEvent = false;
  assert(touched.checkUploadCancellation() && touched.mappedInput.polls == 1);
}
