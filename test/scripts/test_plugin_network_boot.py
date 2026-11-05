#!/usr/bin/env python3
"""Exercise production Plugins and asset-download network resource lifecycles.

The host cannot reproduce the C3 allocator or TLS peak. These checks ensure
hardware enters a fresh boot once, resumes without looping, and leaves with a
full boot after releasing network resources. The simulator stays in-process.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
manager = (ROOT / "src/activities/ActivityManager.cpp").read_text()
plugins = (ROOT / "src/activities/plugins/PluginCatalogActivity.cpp").read_text()
assets = (ROOT / "src/activities/settings/AssetDownloadActivity.cpp").read_text()
boot = (ROOT / "src/main.cpp").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


route = function(manager, "bool ActivityManager::goToPlugins(")
exit_method = function(plugins, "void PluginCatalogActivity::onExit()")
asset_exit = function(assets, "void AssetDownloadActivity::onExit()")
case_start = boot.index("      case NetworkBootTarget::PLUGINS:")
case_end = boot.index("        break;", case_start) + len("        break;")

harness = r'''
#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "SilentRestart.h"
#define LOG_ERR(...) ((void)0)
std::vector<std::string> events;
struct {} renderer;
struct {
  void setReaderTouchscreenOverride(bool enabled) {
    assert(!enabled);
    events.push_back("input");
  }
} mappedInput;
[[maybe_unused]] struct {
  unsigned getFreeHeap() { return 100000; }
  unsigned getMaxAllocHeap() { return 100000; }
} ESP;
[[maybe_unused]] struct {
  void begin(decltype(renderer)&) { events.push_back("fonts"); }
} sdFontSystem;
struct {
  void remove(const char*) { events.push_back("temp"); }
} Storage;
constexpr const char* BROWSE_TMP_PATH = "/catalog.tmp";
struct CatalogActivity {
  void onExit() { events.push_back("wifi"); }
};
struct Activity {
  void onExit() { events.push_back("activity"); }
};
constexpr int WIFI_OFF = 0;
struct {
  void disconnect(bool erase) {
    assert(!erase);
    events.push_back("disconnect");
  }
  void mode(int mode) {
    assert(mode == WIFI_OFF);
    events.push_back("off");
  }
} WiFi;
struct AssetDownloadActivity : Activity {
  struct Popup {
    void clear() { events.push_back("popup"); }
  } popup_;
  struct Catalog {
    ~Catalog() { events.push_back("catalog"); }
  };
  std::unique_ptr<Catalog> catalog_;
  bool catalogReady_ = false;
  void onExit();
};
struct Session {
  void reset() { events.push_back("tls"); }
};
struct PluginCatalogActivity : CatalogActivity {
  PluginCatalogActivity(decltype(renderer)&, decltype(mappedInput)&, bool) {}
  Session session;
  void clearItems() { events.push_back("rows"); }
  void onExit();
};
bool failAllocation = false;
unsigned allocations = 0, restarts = 0;
NetworkBootTarget destination = NetworkBootTarget::OTA;
void silentRestart() { events.push_back("home"); }
void silentRestartToNetwork(NetworkBootTarget target, uint32_t payload) {
  ++restarts;
  destination = target;
  assert(payload == 0);
}
template <class T, class... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  ++allocations;
  if (failAllocation) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}
struct ActivityManager {
  std::unique_ptr<PluginCatalogActivity> pending;
  bool goToPlugins(bool networkBootReady = false);
  void replaceActivity(std::unique_ptr<PluginCatalogActivity> activity) {
    pending = std::move(activity);
  }
} activityManager;
'''

dispatch = """
bool resume(NetworkBootTarget target) {
  bool launched = false;
  switch (target) {
""" + boot[case_start:case_end] + """
    default: break;
  }
  return launched;
}
"""

cases = r'''
int main() {
  assert(isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::PLUGINS)));
  assert(!isNetworkBootTargetValue(0));
  assert(!isNetworkBootTargetValue(10));
  assert(!isNetworkBootTargetValue(UINT32_MAX));
  assert(activityManager.goToPlugins());
#ifndef SIMULATOR
  // The outgoing Home/reader state must not coexist with catalog allocation.
  assert(restarts == 1 && destination == NetworkBootTarget::PLUGINS);
  assert(allocations == 0 && !activityManager.pending);
#else
  assert(restarts == 0 && allocations == 1 && activityManager.pending);
#endif
  allocations = restarts = 0;
  activityManager.pending.reset();
  // Execute main.cpp's actual resume case: it must bypass the restart guard.
  assert(resume(NetworkBootTarget::PLUGINS));
  assert(restarts == 0 && allocations == 1 && activityManager.pending);
  activityManager.pending->onExit();
#ifndef SIMULATOR
  assert((events == std::vector<std::string>{"rows", "tls", "temp", "wifi", "home"}));
#else
  assert((events == std::vector<std::string>{"rows", "tls", "temp", "wifi", "fonts"}));
#endif
  activityManager.pending.reset();
  failAllocation = true;
  // Let setup's existing failure path return Home rather than retry forever.
  assert(!resume(NetworkBootTarget::PLUGINS));
  assert(restarts == 0 && !activityManager.pending);

  // Asset downloads must restore normal boot even if Wi-Fi was cancelled or
  // setup failed before the catalog was allocated/populated.
  for (bool loaded : {false, true}) {
    events.clear();
    AssetDownloadActivity assets;
    if (loaded) assets.catalog_ = std::make_unique<AssetDownloadActivity::Catalog>();
    assets.catalogReady_ = loaded;
    assets.onExit();
    assert(!assets.catalog_ && !assets.catalogReady_);
    std::vector<std::string> expected{"popup"};
    if (loaded) expected.push_back("catalog");
    expected.insert(expected.end(), {"disconnect", "off"});
#ifdef SIMULATOR
    expected.push_back("fonts");
#endif
    expected.insert(expected.end(), {"input", "activity"});
#ifndef SIMULATOR
    expected.push_back("home");
#endif
    assert(events == expected);
  }
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-plugin-boot-") as directory:
    source = Path(directory) / "test.cpp"
    source.write_text(harness + route + "\n" + exit_method + "\n" + asset_exit + "\n" + dispatch + cases)
    for platform, defines in (("hardware", []), ("simulator", ["-DSIMULATOR"])):
        executable = Path(directory) / platform
        subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", *defines,
                        "-I", str(ROOT / "src"), str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print("Plugins and asset-download boot resource restoration passed (hardware and simulator paths).")
