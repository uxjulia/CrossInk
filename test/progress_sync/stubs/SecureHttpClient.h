#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
inline struct TestHeap {
  unsigned free = 45000, largest = 30000;
  unsigned getFreeHeap() const { return free; }
  unsigned getMaxAllocHeap() const { return largest; }
} ESP;
struct TestReply {
  std::string body;
  int status = 200;
  bool complete = true;
};
inline struct TestHttp {
  std::vector<std::string> urls;
  std::vector<TestReply> replies;
  std::string url;
  bool validUrl = true, connected = false;
  size_t streamedBytes = 0;
} testHttp;
namespace freeink {
class SecureHttpClient {
 public:
  using Callback = std::function<bool(const uint8_t*, size_t)>;
  ~SecureHttpClient() { end(); }
  void end() { testHttp.connected = false; }
  void setInsecure() {}
  void setTimeout(unsigned) {}
  bool begin(const std::string& url) {
    testHttp.url = url;
    return testHttp.validUrl;
  }
  template <class Value>
  void addHeader(const char*, const Value&) {}
  int GET(const Callback& callback) {
    const size_t i = testHttp.urls.size();
    testHttp.urls.push_back(testHttp.url);
    const auto& reply = testHttp.replies.at(std::min(i, testHttp.replies.size() - 1));
    testHttp.connected = true;
    status = reply.status;
    complete = reply.complete;
    for (size_t off = 0; off < reply.body.size(); off += 127) {
      size_t n = std::min(size_t{127}, reply.body.size() - off);
      testHttp.streamedBytes += n;
      if (!callback(reinterpret_cast<const uint8_t*>(reply.body.data() + off), n)) {
        complete = false;
        break;
      }
    }
    return status;
  }
  // Unbounded GET/sendRequest overloads intentionally absent: the device code
  // must use response sinks rather than silently accumulate a body.
  int sendRequest(const char*, const uint8_t*, size_t, const Callback& callback) { return GET(callback); }
  int sendRequest(const char*, const std::string&) { return 201; }  // existing account-creation path
  int getStatus() const { return status; }
  bool responseComplete() const { return complete; }

 private:
  bool complete = false;
  int status = 0;
};
}  // namespace freeink
