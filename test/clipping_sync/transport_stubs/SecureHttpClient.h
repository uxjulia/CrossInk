#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
inline struct TestHeap {
  unsigned free = 45000, largest = 30000;
  unsigned getFreeHeap() const { return free; }
  unsigned getMaxAllocHeap() const { return largest; }
} ESP;
inline struct TestHttpReply {
  std::string body, uploaded;
  int status = 200;
  bool complete = true, validUrl = true, connected = false;
  unsigned streamedRequests = 0, timeout = 0;
  void (*onRequest)(const std::string&, const char*) = nullptr;
} testHttp;
namespace freeink {
class SecureHttpClient {
 public:
  using Callback = std::function<bool(const uint8_t*, size_t)>;
  void end() { testHttp.connected = false; }
  void setInsecure() {}
  void setTimeout(unsigned timeout) { testHttp.timeout = timeout; }
  bool begin(const std::string& url) {
    endpoint = url;
    return testHttp.validUrl;
  }
  template <class Value>
  void addHeader(const char*, const Value&) {}
  using Abort = std::function<bool()>;
  int GET(const Callback& callback, const Abort& abort = {}) {
    if (testHttp.onRequest) testHttp.onRequest(endpoint, "GET");
    return receive(callback, abort);
  }
  int receive(const Callback& callback, const Abort& abort) {
    ++testHttp.streamedRequests;
    testHttp.connected = true;
    complete = testHttp.complete;
    for (size_t offset = 0; offset < testHttp.body.size(); offset += 127) {
      if (abort && abort()) {
        complete = false;
        return -1;
      }
      const auto count = std::min(size_t{127}, testHttp.body.size() - offset);
      if (!callback(reinterpret_cast<const uint8_t*>(testHttp.body.data() + offset), count)) {
        complete = false;
        break;
      }
    }
    return testHttp.status;
  }
  int sendRequest(const char*, const uint8_t* payload, size_t length, const Callback& callback,
                  const Abort& abort = {}) {
    testHttp.uploaded.assign(reinterpret_cast<const char*>(payload), length);
    if (testHttp.onRequest) testHttp.onRequest(endpoint, "PUT");
    return receive(callback, abort);
  }
#ifndef CROSSINK_TEST_UPSTREAM_SDK
  int sendRequestStream(const char*, size_t length, size_t (*reader)(void*, size_t, uint8_t*, size_t), void* context,
                        const Callback& callback, const Abort& abort = {}) {
    uint8_t chunk[256];
    testHttp.uploaded.clear();
    for (size_t offset = 0; offset < length;) {
      if (abort && abort()) {
        complete = false;
        return -1;
      }
      const auto count = std::min(sizeof(chunk), length - offset);
      if (reader(context, offset, chunk, count) != count) return -1;
      testHttp.uploaded.append(reinterpret_cast<const char*>(chunk), count);
      offset += count;
    }
    if (testHttp.onRequest) testHttp.onRequest(endpoint, "PUT");
    return receive(callback, abort);
  }
#endif
  bool responseComplete() const { return complete; }

 private:
  std::string endpoint;
  bool complete = false;
};
}  // namespace freeink
