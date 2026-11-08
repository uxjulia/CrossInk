#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using String = std::string;
inline unsigned long millis() {
  static unsigned long value = 0;
  return value++;
}
inline void delay(unsigned) {}
struct EspFake {
  unsigned getFreeHeap() { return 100000; }
  unsigned getMaxAllocHeap() { return 100000; }
};
inline EspFake ESP;
inline constexpr int WL_CONNECTED = 3;
struct WifiFake {
  int status() { return 3; }
  int RSSI() { return -50; }
};
inline WifiFake WiFi;
struct Stream {
  virtual size_t write(const uint8_t*, size_t) = 0;
};
namespace base64 {
inline String encode(const char*) { return "credentials"; }
}  // namespace base64
namespace AppVersion {
inline const char* userAgent() { return "test"; }
}  // namespace AppVersion
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
template <class T>
auto makeUniqueNoThrow(size_t n) {
  return std::unique_ptr<T>(new std::remove_extent_t<T>[n]);
}
inline constexpr int O_WRONLY = 1, O_APPEND = 2;
inline std::map<std::string, std::string> files;
inline bool failWrite = false, failSync = false;
inline uint64_t cardCapacity = 0, cardUsedBytes = 0;
inline size_t fileWriteOpens = 0, capacityChecks = 0;
inline std::set<std::string> failRemovePaths, failRenameSources;
struct FsFile {
  std::string path;
  bool opened = false;
  operator bool() const { return opened; }
  size_t fileSize() const { return files[path].size(); }
  size_t write(const uint8_t* data, size_t n) {
    if (failWrite) return 0;
    files[path].append((const char*)data, n);
    return n;
  }
  bool sync() { return !failSync; }
  bool close() {
    opened = false;
    return true;
  }
};
struct StorageFake {
  uint64_t totalBytes() {
    ++capacityChecks;
    return cardCapacity;
  }
  uint64_t usedBytes() { return cardUsedBytes; }
  bool exists(const char* p) { return files.count(p); }
  bool remove(const char* p) { return !failRemovePaths.count(p) && files.erase(p) > 0; }
  bool rename(const char* a, const char* b) {
    if (failRenameSources.count(a) || !exists(a) || exists(b)) return false;
    files[b] = std::move(files[a]);
    files.erase(a);
    return true;
  }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    f = {p, exists(p)};
    return bool(f);
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    ++fileWriteOpens;
    files[p].clear();
    f = {p, true};
    return true;
  }
  FsFile open(const char* p, int) {
    ++fileWriteOpens;
    return {p, true};
  }
};
inline StorageFake Storage;
struct Reply {
  int status = 200;
  std::string body;
  size_t length = 0;
  std::string range;
  bool complete = true;
  std::string location;
  std::string etag = "\"v1\"";
  std::string disposition;
  std::vector<std::string> extraDispositions;
  size_t chunkSize = 0;
  bool transportFailureAfterHeaders = false;
};
inline std::vector<Reply> replies;
inline size_t nextReply = 0;
inline std::vector<std::map<std::string, std::string>> requests;
inline std::vector<std::string> requestUrls;
inline std::vector<bool> recordReservations;
using esp_err_t = int;
inline constexpr int ESP_OK = 0;
inline constexpr int HTTP_EVENT_ON_HEADER = 1;
struct esp_http_client_event_t {
  int event_id;
  void* user_data;
  const char* header_key;
  const char* header_value;
};
struct esp_http_client_config_t {
  const char* url = nullptr;
  int buffer_size = 0, buffer_size_tx = 0, timeout_ms = 0;
  void (*crt_bundle_attach)() = nullptr;
  bool keep_alive_enable = false;
  esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
  void* user_data = nullptr;
};
struct FakeHttp {
  esp_http_client_config_t config;
  Reply reply;
  size_t position = 0;
  size_t request;
};
using esp_http_client_handle_t = FakeHttp*;
inline const char* esp_err_to_name(int) { return "fake"; }
inline void esp_crt_bundle_attach() {}
inline esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t* config) {
  if (nextReply >= replies.size()) return nullptr;
  auto* h = new FakeHttp{*config, replies[nextReply++], 0, requests.size()};
  requests.emplace_back();
  requestUrls.emplace_back(config->url);
  return h;
}
inline void esp_http_client_set_header(FakeHttp* h, const char* k, const char* v) { requests[h->request][k] = v; }
inline int esp_http_client_open(FakeHttp* h, int) { return h->reply.status < 0 ? -1 : 0; }
inline int64_t esp_http_client_fetch_headers(FakeHttp* h) {
  for (auto kv : {std::pair{"Location", h->reply.location}, std::pair{"Content-Range", h->reply.range},
                  std::pair{"ETag", h->reply.etag}, std::pair{"Content-Disposition", h->reply.disposition}}) {
    if (!kv.second.empty()) {
      esp_http_client_event_t e{1, h->config.user_data, kv.first, kv.second.c_str()};
      h->config.event_handler(&e);
    }
  }
  for (const auto& header : h->reply.extraDispositions) {
    esp_http_client_event_t e{1, h->config.user_data, "Content-Disposition", header.c_str()};
    h->config.event_handler(&e);
  }
  return h->reply.length;
}
inline int esp_http_client_get_status_code(FakeHttp* h) { return h->reply.status; }
inline int esp_http_client_read(FakeHttp* h, char* p, size_t len) {
  auto n = std::min(len, h->reply.body.size() - h->position);
  if (h->reply.chunkSize) n = std::min(n, h->reply.chunkSize);
  memcpy(p, h->reply.body.data() + h->position, n);
  h->position += n;
  return n;
}
inline bool esp_http_client_is_complete_data_received(FakeHttp* h) { return h->reply.complete; }
inline void esp_http_client_cleanup(FakeHttp* h) { delete h; }
inline int esp_http_client_get_and_clear_last_tls_error(FakeHttp*, int*, int*) { return 0; }
namespace freeink {
class SecureHttpClient {
  Reply reply;
  bool stopped = false, wasAborted = false;
  size_t index = 0;

 public:
  void setReserveFullRecord(bool on) { reserveFullRecord = on; }
  bool reserveFullRecord = false;
  void setTimeout(int) {}
  void setInsecure() {}
  void setUserAgent(const char*) {}
  bool begin(const std::string& url) {
    if (nextReply >= replies.size()) return false;
    reply = replies[nextReply++];
    index = requests.size();
    requests.emplace_back();
    requestUrls.push_back(url);
    recordReservations.push_back(reserveFullRecord);
    return true;
  }
  void addHeader(const std::string& k, const std::string& v) { requests[index][k] = v; }
  int GET(const std::function<bool(const uint8_t*, size_t)>& write, const std::function<bool()>& abort) {
    if (abort && abort()) {
      wasAborted = true;
      return reply.status;
    }
    size_t position = 0;
    while (position < reply.body.size()) {
      const size_t count =
          std::min(reply.chunkSize ? reply.chunkSize : reply.body.size(), reply.body.size() - position);
      stopped = !write((const uint8_t*)reply.body.data() + position, count);
      if (stopped) break;
      position += count;
    }
    return reply.transportFailureAfterHeaders ? -1 : reply.status;
  }
  int getStatus() { return reply.status; }
  bool hasContentLength() { return reply.length > 0; }
  size_t getContentLength() { return reply.length; }
  std::string getHeader(const char* key) const {
    return std::string(key) == "content-range"         ? reply.range
           : std::string(key) == "etag"                ? reply.etag
           : std::string(key) == "location"            ? reply.location
           : std::string(key) == "content-disposition" ? reply.disposition
                                                       : std::string{};
  }
  const std::string* getUniqueHeader(const char* key) const {
    if (std::strcmp(key, "content-disposition") != 0 || !reply.extraDispositions.empty() || reply.disposition.empty())
      return nullptr;
    return &reply.disposition;
  }
  bool aborted() { return wasAborted; }
  bool callbackAborted() { return stopped; }
  bool responseComplete() { return reply.complete; }
};
}  // namespace freeink
