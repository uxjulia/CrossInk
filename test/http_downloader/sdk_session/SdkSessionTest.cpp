// Production pinned SDK HTTP framing over a scripted transport, not real TLS.
#include <SecureHttpClient.h>

#include <cassert>
#include <cstdio>

#include "network/HttpSessionFraming.h"
namespace freeink {
SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char*) {}
void SecureClient::setInsecure() {}
int SecureClient::connect(IPAddress ip, uint16_t port) { return _transport.connect(ip, port); }
int SecureClient::connect(const char* host, uint16_t port) { return _transport.connect(host, port); }
size_t SecureClient::write(uint8_t b) { return _transport.write(b); }
size_t SecureClient::write(const uint8_t* p, size_t n) { return _transport.write(p, n); }
int SecureClient::available() { return _transport.available(); }
int SecureClient::read() { return _transport.read(); }
int SecureClient::read(uint8_t* p, size_t n) { return _transport.read(p, n); }
int SecureClient::peek() { return _transport.peek(); }
void SecureClient::flush() {}
void SecureClient::stop() { _transport.stop(); }
uint8_t SecureClient::connected() { return _transport.connected(); }
bool SecureClient::tls13Available() { return false; }
bool hostResolves(const char*) { return true; }
}  // namespace freeink

static void reset(std::initializer_list<std::string> replies) {
  FakeNet::replies().assign(replies);
  FakeNet::requests().clear();
  FakeNet::hosts().clear();
  FakeNet::readDelay() = 0;
  FakeNet::bytesRead() = 0;
  FakeNet::holdOpen() = false;
  fakeNow() = 0;
}
static std::string fixed(const char* body) {
  return std::string("HTTP/1.1 200 OK\r\nContent-Length: ") + std::to_string(std::strlen(body)) + "\r\n\r\n" + body;
}
static void finish(freeink::SecureHttpClient& http) {
  if (!http.responseComplete() ||
      !retainHttpSessionFraming(http.hasContentLength(), http.getHeader("transfer-encoding")))
    http.end();
}
int main() {
  // A complete fixed-length pair can actually reuse the same SDK connection.
  reset({fixed("one") + fixed("two")});
  {
    freeink::SecureHttpClient http;
    http.setReuse(true);
    assert(http.begin("https://fixture/catalog") && http.GET() == 200);
    finish(http);
    assert(http.begin("https://fixture/description") && http.GET() == 200);
    assert(http.getString() == "two" && FakeNet::hosts().size() == 1);
  }
  // Reproduce pinned SDK trailer bug, then verify the app framing gate closes
  // the contaminated socket so the next request reconnects cleanly.
  const auto badTrailer = std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\nX-Long: ") +
                          std::string(4200, 'x') + "\r\n\r\n";
  reset({badTrailer, fixed("clean")});
  {
    freeink::SecureHttpClient http;
    http.setReuse(true);
    assert(http.begin("https://fixture/catalog") && http.GET() == 200);
    assert(http.responseComplete());  // The known pin defect must stay covered.
    finish(http);
    assert(http.begin("https://fixture/description") && http.GET() == 200);
    assert(http.getString() == "clean" && FakeNet::hosts().size() == 2);
  }
  // A stale connection and a short body both reconnect; callback abort closes.
  reset({fixed("old"), fixed("new")});
  {
    freeink::SecureHttpClient http;
    http.setReuse(true);
    assert(http.begin("https://fixture/a") && http.GET() == 200);
    finish(http);
    assert(http.begin("https://fixture/b") && http.GET() == 200);
    assert(http.getString() == "new" && FakeNet::hosts().size() == 2);
  }
  reset({"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort", fixed("ok")});
  {
    freeink::SecureHttpClient http;
    http.setReuse(true);
    assert(http.begin("https://fixture/a") && http.GET() == 200);
    assert(!http.responseComplete());
    finish(http);
    assert(http.begin("https://fixture/b") && http.GET() == 200 && http.getString() == "ok");
  }
  reset({fixed("stop") + fixed("discard"), fixed("clean")});
  {
    freeink::SecureHttpClient http;
    http.setReuse(true);
    assert(http.begin("https://fixture/a") && http.GET([](const uint8_t*, size_t) { return false; }) == 200);
    assert(http.callbackAborted() && !http.responseComplete());
    finish(http);
    assert(http.begin("https://fixture/b") && http.GET() == 200 && http.getString() == "clean");
    assert(FakeNet::hosts().size() == 2);
  }
  std::puts("Pinned SDK session framing, reuse, reconnect and abort checks passed");
}
