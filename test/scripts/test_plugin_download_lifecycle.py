"""Compile production catalog exit and download methods with controlled heap/Wi-Fi."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/activities/CatalogActivity.cpp").read_text()
header = (ROOT / "src/activities/CatalogActivity.h").read_text()
exit_method = "void CatalogActivity::onExit()" + source.split(
    "void CatalogActivity::onExit()", 1
)[1].split("bool CatalogActivity::wifiConnected()", 1)[0]
download_method = "HttpDownloader::DownloadError CatalogActivity::downloadFile(" + source.split(
    "HttpDownloader::DownloadError CatalogActivity::downloadFile(", 1
)[1].split("void CatalogActivity::finishDownload(", 1)[0]
download_declaration = "HttpDownloader::DownloadError downloadFile(" + header.split(
    "HttpDownloader::DownloadError downloadFile(", 1
)[1].split(";", 1)[0] + ";"

harness = r"""
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#define LOG_ERR(...) ((void)0)
std::vector<std::string> events;
struct Heap {
  uint32_t free = 0, largest = 0;
  uint32_t getFreeHeap() { return free; }
  uint32_t getMaxAllocHeap() { return largest; }
} ESP;
struct FontCacheManager {
  void releaseSdFontCaches() { events.push_back("cache"); }
};
struct Renderer {
  FontCacheManager cache;
  FontCacheManager* getFontCacheManager() { return &cache; }
};
struct Activity { void onExit() { events.push_back("exit"); } };
constexpr int WIFI_MODE_NULL = 0, WIFI_OFF = 0;
struct Wifi {
  int current = 0;
  int getMode() { return current; }
  void disconnect(bool) { events.push_back("disconnect"); }
  void mode(int value) { current = value; events.push_back("off"); }
} WiFi;
void delay(int value) { assert(value == 30); events.push_back("delay"); }
// A returning restart also models deep sleep suppressing the real helper.
void silentRestart() { events.push_back("restart"); }
struct RenderLock { RenderLock() {} };
struct HttpDownloader {
  enum DownloadError { OK, HTTP_ERROR };
  enum class Transport { ESP_HTTP, WOLFSSL };
  using Header = std::pair<std::string, std::string>;
  struct DownloadOptions {
    Transport transport = Transport::ESP_HTTP;
    bool stageAsPart = false, checkFreeSpace = false;
    const std::vector<Header>* headers = nullptr;
    std::function<bool()> shouldCancel;
  };
  static Transport lastTransport;
  static DownloadError downloadToFile(const std::string& url, const std::string& dest,
      std::function<void(size_t, size_t)> progress, const bool* cancel,
      const std::string& user, const std::string& password, const DownloadOptions& options) {
    events.push_back("download");
    lastTransport = options.transport;
    assert(url == "https://example.invalid/file" && dest == "/file");
    assert(user == "user" && password == "password");
    assert(options.stageAsPart && options.checkFreeSpace);
    assert(options.headers && options.headers->size() == 1);
    assert(options.headers->front().first == "X-Test");
    assert(cancel && !*cancel && !options.shouldCancel());
    progress(10, 20);
    return OK;
  }
};
HttpDownloader::Transport HttpDownloader::lastTransport;
struct CatalogActivity : Activity {
  Renderer renderer;
  size_t downloadProgress = 99, downloadTotal = 99;
  int lastRenderedPercent = 99;
  unsigned long lastProgressUpdateMs = 99;
  bool cancelDownload = false;
  void onExit();
  void onDownloadProgress(size_t done, size_t total) { downloadProgress = done; downloadTotal = total; }
""" + download_declaration + r"""
};
""" + exit_method + download_method + r"""
int main() {
  using T = HttpDownloader::Transport;
  CatalogActivity catalog;
  for (int mode : {0, 1}) {
    WiFi.current = mode;
    events.clear();
    catalog.onExit();
    if (!mode) assert(events == std::vector<std::string>{"exit"});
    else {
#ifdef SIMULATOR
      assert((events == std::vector<std::string>{"exit", "disconnect", "delay", "off"}));
#else
      assert((events == std::vector<std::string>{"exit", "disconnect", "delay", "restart", "off"}));
#endif
      assert(WiFi.current == WIFI_OFF);
    }
  }
  const std::vector<HttpDownloader::Header> headers{{"X-Test", "value"}};
  for (T transport : {T::WOLFSSL, T::ESP_HTTP}) {
    const uint32_t minFree = transport == T::WOLFSSL ? 40000 : 65536;
    const uint32_t minLargest = transport == T::WOLFSSL ? 20000 : 32768;
    for (const auto memory : {std::pair{minFree - 1, minLargest}, std::pair{minFree, minLargest - 1},
                             std::pair{minFree, minLargest}, std::pair{65536u, 32768u}}) {
      ESP.free = memory.first;
      ESP.largest = memory.second;
      events.clear();
      const auto result = catalog.downloadFile("https://example.invalid/file", "/file", "user", "password", headers,
                                               transport);
      assert(!events.empty() && events.front() == "cache");
      const bool admitted = memory.first >= minFree && memory.second >= minLargest;
      assert(result == (admitted ? HttpDownloader::OK : HttpDownloader::HTTP_ERROR));
      if (admitted) {
        assert((events == std::vector<std::string>{"cache", "download"}));
        assert(HttpDownloader::lastTransport == transport);
        assert(catalog.downloadProgress == 10 && catalog.downloadTotal == 20);
      } else assert(events.size() == 1);
    }
  }
  ESP.free = 40000;
  ESP.largest = 20000;
  assert(catalog.downloadFile("https://example.invalid/file", "/file", "user", "password", headers) == HttpDownloader::OK);
  assert(HttpDownloader::lastTransport == T::WOLFSSL);
}
"""

with tempfile.TemporaryDirectory(prefix="plugin-download-lifecycle-") as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(harness)
    for defines in ([], ["-DSIMULATOR"]):
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *defines,
                        str(path / "test.cpp"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("Plugin download heap boundaries and exit lifecycle passed (hardware and simulator)")
