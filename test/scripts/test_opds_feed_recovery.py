#!/usr/bin/env python3
"""Run production OPDS fetch/cleanup with real Expat and scripted HTTP failures.

The screen and transport are doubles; this does not model ESP32 Wi-Fi memory.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/activities/browser/OpdsBookBrowserActivity.cpp").read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


prefix = r'''
#include <OpdsParser.h>
#include <OpdsStream.h>
#include <XmlParserUtils.h>
#include "util/OpdsFeedError.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
struct RenderLock {};
namespace UrlUtils {
std::string buildUrl(const std::string& base, const std::string& path) { return path.empty() ? base : path; }
std::string ensureProtocol(const std::string& url) { return url; }
std::string forLog(const std::string& url) { return url; }
}
void replaceFolderEmoji(std::string&) {}
constexpr const char *STR_MEMORY_ERROR = "memory", *STR_NO_SERVER_URL = "no server",
  *STR_OPDS_FEED_BUFFER_MEMORY_ERROR = "buffer memory", *STR_PARSE_FEED_FAILED = "parse",
  *STR_FEED_UNAUTHORIZED = "unauthorized", *STR_FEED_REDIRECTED = "redirected",
  *STR_FEED_SERVER_ERROR = "server error", *STR_FEED_UNREACHABLE = "unreachable",
  *STR_FETCH_FEED_FAILED = "fetch", *STR_PREV_PAGE = "previous", *STR_NEXT_PAGE = "next",
  *STR_NO_ENTRIES = "empty";
const char* tr(const char* text) { return text; }
template<class T> std::unique_ptr<T> makeUniqueNoThrow(size_t n) {
  return std::unique_ptr<T>(new (std::nothrow) OpdsEntry[n]);
}
struct HttpDownloader {
  enum DownloadError { OK, HTTP_ERROR, UNAUTHORIZED, FILE_ERROR };
  enum class Transport { WOLFSSL };
  struct DownloadOptions { Transport transport; std::string authorizationOrigin; };
  static inline std::string response;
  static inline DownloadError outcome = OK;
  static inline int status = 200;
  static inline bool withheld = false;
  static inline size_t delivered = 0;
  static DownloadError streamUrl(const std::string&, const std::function<bool(const uint8_t*, size_t)>& cb,
                                std::nullptr_t, const std::string&, const std::string&, DownloadOptions,
                                HttpRequestDiagnostics* diagnostics) {
    *diagnostics = {status, true, withheld, false};
    delivered = 0;
    for (size_t offset = 0; offset < response.size(); offset += 37) {
      const size_t n = std::min(size_t{37}, response.size() - offset);
      delivered += n;
      if (!cb(reinterpret_cast<const uint8_t*>(response.data() + offset), n)) {
        diagnostics->callbackRejected = true;
        return FILE_ERROR;
      }
    }
    return outcome;
  }
};
struct OpdsBookBrowserActivity {
  enum class BrowserState { ERROR, BROWSING };
  static constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = MAX_OPDS_FEED_ENTRIES + 2;
  std::unique_ptr<OpdsEntry[]> entries;
  size_t entryCount = 0;
  bool uiReady = true, hasPrevPageRow = false, hasNextPageRow = false;
  int selectorIndex = 0, topIndex = 0;
  BrowserState state = BrowserState::ERROR;
  std::string errorMessage, searchTemplate;
  struct { std::string url = "http://opds.example/recent", username, password; } server;
  struct Input {
    std::string withPreviousPageArrow(const char* text) { return text; }
    std::string withNextPageArrow(const char* text) { return text; }
    std::string resolveLabel(const std::string& text) { return text; }
  } mappedInput;
  void requestUpdate() {}
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&&);
  void fetchFeed(const std::string&);
};
'''
main = r'''
void assertEmpty(const OpdsBookBrowserActivity& browser) {
  assert(browser.entryCount == 0);
  assert(!browser.uiReady && !browser.hasNextPageRow && !browser.hasPrevPageRow);
  for (size_t i = 0; i < browser.OPDS_BROWSER_ENTRY_CAPACITY; ++i) {
    const auto& entry = browser.entries[i];
    assert(entry.title.empty() && entry.author.empty() && entry.href.empty() && entry.id.empty());
    assert(entry.description[0] == '\0');
  }
}
int main() {
  const std::string book = "<entry><title>A recent book with a short title</title>"
    "<author><name>Example Author</name></author>"
    "<link href='/book.epub' rel='http://opds-spec.org/acquisition' type='application/epub+zip'/>"
    "<summary>" + std::string(1700, 'd') + "</summary></entry>";
  OpdsBookBrowserActivity browser;
  assert(browser.ensureEntryBuffer());
  // Reproduce partial entries with an unpublished browser count, including
  // the two pagination slots. Clearing must release every populated slot.
  browser.entries[browser.OPDS_BROWSER_ENTRY_CAPACITY - 1].title = std::string(200, 'p');
  browser.entries[10].href = std::string(200, 'h');
  browser.clearEntries();
  assertEmpty(browser);

  std::string full = "<feed>";
  for (size_t i = 0; i < MAX_OPDS_FEED_ENTRIES; ++i) full += book;
  full += "<link rel='previous' href='/recent?page=0'/><link rel='next' href='/recent?page=2'/></feed>";
  for (int attempt = 0; attempt < 3; ++attempt) {
    HttpDownloader::response = "<feed>" + book + book;
    HttpDownloader::outcome = HttpDownloader::HTTP_ERROR;
    browser.fetchFeed("");
    assert(browser.state == OpdsBookBrowserActivity::BrowserState::ERROR);
    assert(browser.errorMessage == STR_FETCH_FEED_FAILED);
    assertEmpty(browser);

    // A well-formed body with a failed HTTP transfer must also be discarded.
    HttpDownloader::response = full;
    browser.fetchFeed("");
    assertEmpty(browser);

    HttpDownloader::outcome = HttpDownloader::OK;
    browser.fetchFeed("");
    assert(browser.state == OpdsBookBrowserActivity::BrowserState::BROWSING);
    assert(browser.entryCount == browser.OPDS_BROWSER_ENTRY_CAPACITY);
    assert(browser.hasNextPageRow && browser.hasPrevPageRow);
    assert(browser.entries[1].title == "A recent book with a short title");
    assert(browser.entries[1].description[0] == 'd');
    assert(browser.entries[0].href == "/recent?page=0");
    assert(browser.entries[browser.entryCount - 1].href == "/recent?page=2");
  }
  // Parser errors abort the transport and retain their own error message.
  HttpDownloader::response = "<feed>" + book + "</invalid>" + std::string(10000, 'x');
  browser.fetchFeed("");
  assert(browser.errorMessage == STR_PARSE_FEED_FAILED);
  assert(HttpDownloader::delivered < HttpDownloader::response.size());
  assertEmpty(browser);

  HttpDownloader::response = full;
  XmlTestHeap::limit = 0;
  browser.fetchFeed("");
  XmlTestHeap::limit = SIZE_MAX;
  assert(browser.errorMessage == STR_MEMORY_ERROR);
  assertEmpty(browser);
  // Force a buffer failure after construction via one oversized XML token.
  HttpDownloader::response = "<feed><entry attr='" + std::string(40000, 'x') + "'/></feed>";
  XmlTestHeap::limit = 16 * 1024;
  browser.fetchFeed("");
  XmlTestHeap::limit = SIZE_MAX;
  assert(browser.errorMessage == STR_OPDS_FEED_BUFFER_MEMORY_ERROR);
  assert(HttpDownloader::delivered < HttpDownloader::response.size());
  assertEmpty(browser);
  assert(XmlTestHeap::live == 0);

  for (const auto status : {401, 503, 0}) {
    HttpDownloader::status = status;
    HttpDownloader::outcome = status == 401 ? HttpDownloader::UNAUTHORIZED : HttpDownloader::HTTP_ERROR;
    HttpDownloader::response.clear();
    browser.fetchFeed("");
    assert(browser.errorMessage == (status == 401 ? STR_FEED_UNAUTHORIZED :
                                  status == 503 ? STR_FEED_SERVER_ERROR : STR_FEED_UNREACHABLE));
    assertEmpty(browser);
  }
  HttpDownloader::status = 200;
  HttpDownloader::outcome = HttpDownloader::OK;
  HttpDownloader::response = full;
  browser.fetchFeed("");
  assert(browser.entryCount == browser.OPDS_BROWSER_ENTRY_CAPACITY);
  assert(XmlTestHeap::live == 0);
  std::puts("OPDS partial-feed cleanup, retry, pagination, parser OOM and HTTP errors passed");
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-opds-recovery-") as temp:
    temp = Path(temp)
    harness = temp / "recovery.cpp"
    harness.write_text(prefix + "\n".join(function(signature) for signature in (
        "void OpdsBookBrowserActivity::fetchFeed(",
        "bool OpdsBookBrowserActivity::ensureEntryBuffer(",
        "void OpdsBookBrowserActivity::clearEntries(",
        "bool OpdsBookBrowserActivity::appendEntry(",
    )) + main)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
             "-DXML_CONTEXT_BYTES=0", "-DXML_GE=0"]
    includes = ["-I" + str(root / p) for p in (
        "test/opds_parser/stubs", "lib/OpdsParser", "lib/XmlParserUtils", "lib/expat", "src")]
    objects = []
    for name in ("xmlparse", "xmlrole", "xmltok"):
        obj = temp / (name + ".o")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", *flags, *includes,
                        "-c", str(root / "lib/expat" / (name + ".c")), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = temp / "recovery"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", *flags, *includes,
                    str(harness), str(root / "lib/OpdsParser/OpdsParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsStream.cpp"), *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
