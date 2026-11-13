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
#include <OpdsFeedParser.h>
#include <OpdsSearchTemplate.h>
#include <OpenSearchDescParser.h>
#include "network/HttpRedirectPolicy.h"
#include <map>
#include "util/UrlUtils.h"
#include <vector>
#include <Memory.h>
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
struct I18n { static I18n& getInstance() { static I18n value; return value; } const char* getCode() const { return "en"; } };
void replaceFolderEmoji(std::string&) {}
constexpr const char *STR_MEMORY_ERROR = "memory", *STR_NO_SERVER_URL = "no server",
  *STR_OPDS_FEED_BUFFER_MEMORY_ERROR = "buffer memory", *STR_PARSE_FEED_FAILED = "parse",
  *STR_FEED_UNAUTHORIZED = "unauthorized", *STR_FEED_REDIRECTED = "redirected",
  *STR_FEED_SERVER_ERROR = "server error", *STR_FEED_UNREACHABLE = "unreachable",
  *STR_FETCH_FEED_FAILED = "fetch", *STR_PREV_PAGE = "previous", *STR_NEXT_PAGE = "next",
  *STR_NO_ENTRIES = "empty";
const char* tr(const char* text) { return text; }
struct HttpDownloader {
  enum DownloadError { OK, HTTP_ERROR, UNAUTHORIZED, FILE_ERROR };
  enum class Transport { WOLFSSL };
  struct Session { void reset() {} };
  struct DownloadOptions { Transport transport; std::string_view authorizationOrigin; Session* session = nullptr;
    std::string* finalUrl = nullptr; size_t maxBytes = 0; };
  static inline std::map<std::string, std::pair<std::string,std::string>> documents;
  static inline std::vector<std::string> urls;
  static inline std::string response, requestedUrl;
  static inline DownloadError outcome = OK;
  static inline int status = 200;
  static inline bool withheld = false;
  static inline size_t delivered = 0;
  static DownloadError streamUrl(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& cb,
                                std::nullptr_t, const std::string&, const std::string&, DownloadOptions options,
                                HttpRequestDiagnostics* diagnostics = nullptr) {
    requestedUrl = url;
    urls.push_back(url);
    const auto it = documents.find(url);
    const auto& payload = it == documents.end() ? response : it->second.first;
    if (options.finalUrl) *options.finalUrl = it == documents.end() ? url : it->second.second;
    assert(options.authorizationOrigin == "http://opds.example/recent");
    HttpRequestDiagnostics local;
    if (!diagnostics) diagnostics = &local;
    *diagnostics = {}; diagnostics->status = status; diagnostics->attempted = true; diagnostics->authorizationWithheld = withheld;
    delivered = 0;
    for (size_t offset = 0; offset < payload.size(); offset += 37) {
      const size_t n = std::min(size_t{37}, payload.size() - offset);
      delivered += n;
      if (!cb(reinterpret_cast<const uint8_t*>(payload.data() + offset), n)) {
        diagnostics->callbackRejected = true;
        return FILE_ERROR;
      }
    }
    return outcome;
  }
};
struct CatalogDouble {
  auto get(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& body,
           const std::string& user, const std::string& password, HttpDownloader::DownloadOptions options,
           HttpRequestDiagnostics* diagnostics = nullptr) {
    return HttpDownloader::streamUrl(url, body, nullptr, user, password, options, diagnostics);
  }
};
struct OpdsBookBrowserActivity {
  CatalogDouble catalogClient;
  enum class BrowserState { ERROR, BROWSING };
  static constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = MAX_OPDS_FEED_ENTRIES + 2;
  std::unique_ptr<OpdsEntry[]> entries;
  size_t entryCount = 0;
  bool uiReady = true, hasPrevPageRow = false, hasNextPageRow = false;
  int selectorIndex = 0, topIndex = 0;
  BrowserState state = BrowserState::ERROR;
  std::string errorMessage, searchTemplate, searchBaseUrl, currentPath;
  std::vector<std::string> navigationHistory;
  void showLoadingBeforeFetch() {}
  void performSearch(const std::string&);
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
  bool loadPublication(OpdsEntry&, const std::string&);
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
  HttpDownloader::response = R"({"navigation":[{"title":"JSON folder","href":"next"}],"links":[{"rel":"next","href":"page2"}]})";
  browser.fetchFeed("");
  assert(browser.entryCount == 2 && browser.hasNextPageRow);
  assert(browser.entries[0].title == "JSON folder");
  HttpDownloader::response = R"({"navigation":[{"title":"Partial JSON folder","href":"next"}],)";
  browser.fetchFeed("");
  assert(browser.errorMessage == STR_PARSE_FEED_FAILED);
  assertEmpty(browser);
  HttpDownloader::response = R"({"navigation":[{"title":"JSON retry","href":"next"}]})";
  browser.fetchFeed("");
  assert(browser.entryCount == 1 && browser.entries[0].title == "JSON retry");
  OpdsEntry selected;
  selected.title = "Feed title";
  selected.href = "old.epub";
  selected.selfHref = "details.json";
  HttpDownloader::response = R"({"metadata":{"title":"Publication title","description":"Details text"},"links":[{"rel":"download","type":"application/epub+zip","href":"new.epub"}]})";
  assert(browser.loadPublication(selected, "http://opds.example/details.json"));
  assert(selected.title == "Publication title" && selected.href == "http://opds.example/new.epub" && selected.selfHref.empty());
  assert(std::string(selected.description.data()) == "Details text");
  HttpDownloader::response = "{\"metadata\":{";
  assert(!browser.loadPublication(selected, "http://opds.example/details.json"));
  assert(selected.title == "Publication title" && selected.href == "http://opds.example/new.epub");
  HttpDownloader::outcome = HttpDownloader::OK;
  HttpDownloader::status = 200;
  HttpDownloader::response = R"({"navigation":[{"title":"Result","href":"result"}]})";
  browser.searchBaseUrl.clear();
  browser.currentPath = "/catalog/fiction/index.json?page=1";
  browser.searchTemplate = "search{?query,title,author}";
  browser.performSearch("a & café");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/fiction/search?query=a%20%26%20caf%C3%A9");
  assert(browser.navigationHistory.back() == "/catalog/fiction/index.json?page=1");
  browser.searchBaseUrl.clear();
  browser.currentPath = "/catalog/fiction/index.json?page=1";
  browser.searchTemplate = "{?query}";
  browser.performSearch("book");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/fiction/index.json?query=book");
  browser.searchBaseUrl.clear();
  browser.searchTemplate = "../search?q={searchTerms}&page={startPage?}";
  browser.performSearch("new book");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/search?q=new%20book&page=");
  HttpDownloader::response = "<feed>" + book + "<link rel='search' type='application/opensearchdescription+xml' href='../search/description.xml'/></feed>";
  HttpDownloader::documents["http://opds.example/search/description.xml"] = {
    "<OpenSearchDescription><Url type='application/atom+xml' template='results?q={searchTerms}'/></OpenSearchDescription>",
    "http://opds.example/redirected/description.xml"};
  browser.fetchFeed("http://opds.example/catalog/index.xml");
  assert(browser.entryCount == 1 && browser.state == OpdsBookBrowserActivity::BrowserState::BROWSING);
  assert(browser.searchTemplate == "results?q={searchTerms}");
  assert(browser.searchBaseUrl == "http://opds.example/redirected/description.xml");
  HttpDownloader::response = "<feed>" + book + "</feed>";
  browser.performSearch("a & café");
  assert(HttpDownloader::requestedUrl == "http://opds.example/redirected/results?q=a%20%26%20caf%C3%A9");
  HttpDownloader::response = "<feed>" + book + "<link rel='search' type='application/opensearchdescription+xml' href='../search/description.xml'/></feed>";
  HttpDownloader::documents["http://opds.example/search/description.xml"].first = "<invalid>";
  browser.fetchFeed("http://opds.example/catalog/index.xml");
  assert(browser.entryCount == 1 && browser.searchTemplate.empty());
  HttpDownloader::documents.clear();
  const auto callsBefore = HttpDownloader::urls.size();
  HttpDownloader::response = "<feed>" + book + "<link rel='search' type='application/opensearchdescription+xml' href='http://opds.example/insecure'/></feed>";
  browser.fetchFeed("https://opds.example/catalog/index.xml");
  assert(HttpDownloader::urls.size() == callsBefore + 1 && browser.searchTemplate.empty());
  std::puts("OPDS linked OpenSearch, Atom/JSON cleanup, retry, pagination, parser OOM, publication fetch and HTTP errors passed");
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-opds-recovery-") as temp:
    temp = Path(temp)
    harness = temp / "recovery.cpp"
    harness.write_text(prefix + "\n".join(function(signature) for signature in (
        "void OpdsBookBrowserActivity::fetchFeed(",
        "void OpdsBookBrowserActivity::performSearch(",
        "bool OpdsBookBrowserActivity::loadPublication(",
        "bool OpdsBookBrowserActivity::ensureEntryBuffer(",
        "void OpdsBookBrowserActivity::clearEntries(",
        "bool OpdsBookBrowserActivity::appendEntry(",
    )) + main)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
             "-DXML_CONTEXT_BYTES=0", "-DXML_GE=0"]
    includes = ["-I" + str(root / p) for p in (
        "test/opds_parser/stubs", "lib/OpdsParser", "lib/XmlParserUtils", "lib/expat", "lib/Memory", "freeink-sdk/libs/network/JsonSax/include", "src")]
    objects = []
    for name in ("xmlparse", "xmlrole", "xmltok"):
        obj = temp / (name + ".o")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", *flags, *includes,
                        "-c", str(root / "lib/expat" / (name + ".c")), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = temp / "recovery"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", *flags, *includes,
                    str(harness), str(root / "lib/OpdsParser/OpdsParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsFeedParser.cpp"),
                    str(root / "lib/OpdsParser/OpenSearchDescParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsJsonParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsSearchTemplate.cpp"), str(root / "src/util/UrlUtils.cpp"),
                    str(root / "freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp"), *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
