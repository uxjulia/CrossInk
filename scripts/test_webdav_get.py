#!/usr/bin/env python3
"""Run the production WebDAV GET body with the pinned SDK streamer and host I/O."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "freeink-sdk/libs/hardware/SDCardManager"


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


FIXTURE = r'''
#include <SDCardManager.h>
#include <cassert>
#include <memory>
#include <limits>
struct Socket {
  std::vector<uint8_t> bytes;
  size_t maxWrite = SIZE_MAX, stopAt = SIZE_MAX;
  unsigned byteWrites = 0;
  bool stopped = false;
};
class NetworkClient : public Print {
 public:
  std::shared_ptr<Socket> socket;
  explicit NetworkClient(std::shared_ptr<Socket> s) : socket(std::move(s)) {}
  size_t write(const uint8_t* data, size_t count) override {
    if (socket->bytes.size() >= socket->stopAt) return 0;
    nowMs += 5;
    const size_t written = std::min({count, socket->maxWrite, socket->stopAt - socket->bytes.size()});
    socket->bytes.insert(socket->bytes.end(), data, data + written);
    return written;
  }
  size_t write(uint8_t c) { ++socket->byteWrites; return write(&c, 1); }
  void stop() { socket->stopped = true; }
};
struct HalFile {
  bool open = true, directory = false;
  operator bool() const { return open; }  // Keep the dangerous old implicit conversion.
  size_t size() const { return fakeSd::advertisedSize; }
  bool isDirectory() const { return directory; }
  void close();
};
struct StorageFixture {
  SDCardManager backend;
  bool existsResult = true, directory = false, metadataOpen = false, streamFailed = false;
  unsigned streams = 0;
  bool exists(const char*) const { return existsResult; }
  HalFile open(const char*) { metadataOpen = !fakeSd::openFails; return {metadataOpen, directory}; }
  bool readFileToStream(const char* path, Print& out) {
    assert(!metadataOpen);  // The real SD cannot reopen this path while it is held.
    ++streams;
    const bool result = backend.readFileToStream(path, out);
    streamFailed = !result;
    return result;
  }
} Storage;
void HalFile::close() { open = false; Storage.metadataOpen = false; }
unsigned errors = 0;
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) (++errors)
struct WebServer {
  String path = "/book.bin";
  bool protectedPath = false, failReopen = false;
  size_t contentLength = SIZE_MAX;
  int status = 0;
  std::shared_ptr<Socket> socket = std::make_shared<Socket>();
  void setContentLength(size_t size) { contentLength = size; }
  void send(int code, const char*, const char*) { status = code; if (code == 200 && failReopen) fakeSd::openFails = true; }
  NetworkClient client() { return NetworkClient(socket); }
};
struct WebDAVHandler {
  String currentPath;
  bool protectedPath = false;
  String getRequestPath(WebServer& s) { protectedPath = s.protectedPath; return s.path; }
  bool isProtectedPath(const String&) const { return protectedPath; }
  String getMimeType(const String&) const { return "application/octet-stream"; }
  void handleGet(WebServer&);
};
void reset(size_t size) {
  fakeSd::data.resize(size);
  for (size_t i = 0; i < size; ++i) fakeSd::data[i] = static_cast<uint8_t>(i * 31 + i / 256);
  fakeSd::advertisedSize = size;
  fakeSd::openFails = false;
  fakeSd::failAt = fakeSd::maxRead = SIZE_MAX;
  fakeSd::closes = fakeSd::reads = fakeSd::readMs = 0;
  nowMs = yields = watchdogResets = errors = 0;
  watchdogSubscribed = true;
  Storage.existsResult = true;
  Storage.directory = Storage.metadataOpen = Storage.streamFailed = false;
  Storage.streams = 0;
}
'''
TESTS = r'''
int main() {
  assert(Storage.backend.begin());
  WebDAVHandler handler;
  for (size_t size : {size_t(0), size_t(1), size_t(54822), size_t(1249290)}) {
    for (size_t maxWrite : {SIZE_MAX, size_t(7)}) {
      reset(size);
      WebServer server;
      server.socket->maxWrite = maxWrite;
      handler.handleGet(server);
      assert(server.status == 200 && server.contentLength == size);
      assert(server.socket->bytes == fakeSd::data);
      assert(server.socket->byteWrites == 0 && !server.socket->stopped);
      assert(Storage.streams == 1 && fakeSd::closes == 1 && !Storage.metadataOpen && errors == 0);
      if (size >= 54822) assert(yields > 0 && watchdogResets > 0);
    }
  }
  reset(769);
  WebServer disconnected;
  disconnected.socket->stopAt = 300;
  handler.handleGet(disconnected);
  assert(disconnected.socket->bytes.size() == 300 && disconnected.socket->stopped);
  assert(Storage.streamFailed && errors == 1 && fakeSd::closes == 1);
  reset(769);
  fakeSd::failAt = 256;
  WebServer readError;
  handler.handleGet(readError);
  assert(readError.socket->bytes.size() == 256 && readError.socket->stopped && errors == 1);
  assert(fakeSd::closes == 1);
  reset(769);
  fakeSd::advertisedSize = 800;
  WebServer truncated;
  handler.handleGet(truncated);
  assert(truncated.contentLength == 800 && truncated.socket->bytes.size() == 769);
  assert(truncated.socket->stopped && fakeSd::closes == 1);
  reset(769);
  WebServer reopenFailure;
  reopenFailure.failReopen = true;
  handler.handleGet(reopenFailure);
  assert(reopenFailure.status == 200 && reopenFailure.socket->stopped && errors == 1);
  assert(fakeSd::closes == 0 && !Storage.metadataOpen && Storage.streamFailed);
  for (int failure = 0; failure < 4; ++failure) {
    reset(10);
    WebServer server;
    if (failure == 0) server.protectedPath = true;
    if (failure == 1) Storage.existsResult = false;
    if (failure == 2) fakeSd::openFails = true;
    if (failure == 3) Storage.directory = true;
    handler.handleGet(server);
    assert(server.status == (failure == 0 ? 403 : failure == 1 ? 404 : failure == 2 ? 500 : 405));
    assert(Storage.streams == 0 && server.socket->bytes.empty() && !Storage.metadataOpen);
  }
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-webdav-") as directory:
    source = Path(directory) / "test.cpp"
    source.write_text(FIXTURE + method((ROOT / "src/network/WebDAVHandler.cpp").read_text(),
                                      "void WebDAVHandler::handleGet(WebServer& s)") + TESTS)
    binary = Path(directory) / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(SDK / "test/host/stubs"), "-I" + str(SDK / "include"),
                    str(source), str(SDK / "src/SDCardManager.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("WebDAV GET: binary/empty/large/short-write/disconnect/read-failure/guards passed (ASan/UBSan)")
