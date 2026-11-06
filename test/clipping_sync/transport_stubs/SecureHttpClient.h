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
  unsigned streamedRequests = 0;
} testHttp;
namespace freeink {
class SecureHttpClient {
 public:
  using Callback = std::function<bool(const uint8_t*, size_t)>;
  void end() { testHttp.connected = false; }
  void setInsecure() {}
  void setTimeout(unsigned) {}
  bool begin(const std::string&) { return testHttp.validUrl; }
  template <class Value>
  void addHeader(const char*, const Value&) {}
  int GET(const Callback& callback) {
    ++testHttp.streamedRequests;
    testHttp.connected = true;
    complete = testHttp.complete;
    for (size_t offset = 0; offset < testHttp.body.size(); offset += 127) {
      const auto count = std::min(size_t{127}, testHttp.body.size() - offset);
      if (!callback(reinterpret_cast<const uint8_t*>(testHttp.body.data() + offset), count)) {
        complete = false;
        break;
      }
    }
    return testHttp.status;
  }
  int sendRequest(const char*, const uint8_t*, size_t, const Callback& callback) { return GET(callback); }
  int sendRequestStream(const char*, size_t length, size_t (*reader)(void*, size_t, uint8_t*, size_t), void* context,
                        const Callback& callback) {
    uint8_t chunk[256];
    testHttp.uploaded.clear();
    for (size_t offset = 0; offset < length;) {
      const auto count = std::min(sizeof(chunk), length - offset);
      if (reader(context, offset, chunk, count) != count) return -1;
      testHttp.uploaded.append(reinterpret_cast<const char*>(chunk), count);
      offset += count;
    }
    return GET(callback);
  }
  bool responseComplete() const { return complete; }

 private:
  bool complete = false;
};
}  // namespace freeink
