#include "PluginLocations.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace PluginLocations {

std::vector<Entry> scanPluginsOnDisk() {
  std::vector<Entry> plugins;
  plugins.reserve(MAX_PLUGINS);
  unsigned scanned = 0;
  for (size_t r = 0; r < ROOT_COUNT; r++) {
    HalFile root = Storage.open(ROOTS[r]);
    if (!root || !root.isDirectory()) {
      root.close();
      continue;
    }
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      char name[128];
      const bool directory = entry.isDirectory();
      const auto length = entry.getName(name, sizeof(name));
      entry.close();
      if (++scanned > 128) break;
      if (!directory || length == 0 || length >= sizeof(name) - 1 || !validName(name)) continue;
      Entry e;
      e.name = name;
      e.dir = std::string(ROOTS[r]) + "/" + name;
      // Earlier roots claim a name even when that folder has no marker files.
      if (findPluginDirOnDisk(name) != e.dir) continue;
      e.hasPluginJs = Storage.exists((e.dir + "/plugin.js").c_str());
      e.hasDevice = Storage.exists((e.dir + "/device.json").c_str());
      e.hasManifest = Storage.exists((e.dir + "/manifest.json").c_str());
      if (e.hasPluginJs || e.hasDevice || e.hasManifest) plugins.push_back(std::move(e));
      if (plugins.size() >= MAX_PLUGINS) break;
    }
    const bool failed = FsHelpers::directoryIterationFailed(root);
    root.close();
    if (failed) LOG_ERR("PLUG", "Plugin directory read failed");
    if (plugins.size() >= MAX_PLUGINS || scanned > 128) {
      LOG_INF("PLUG", "Plugin scan limit reached");
      break;
    }
  }
  return plugins;
}

std::string findPluginDirOnDisk(const char* name) {
  if (!name || !validName(name)) return {};
  for (size_t i = 0; i < ROOT_COUNT; i++) {
    std::string dir = std::string(ROOTS[i]) + "/" + name;
    HalFile folder = Storage.open(dir.c_str());
    const bool found = folder && folder.isDirectory();
    folder.close();
    if (found) return dir;
  }
  return {};
}

bool isPluginQuarantined(const std::string& dir) { return Storage.exists((dir + "/" + INSTALL_MARKER).c_str()); }
std::vector<Entry> scanPlugins() {
  if (!SETTINGS.pluginsEnabled) return {};
  auto plugins = scanPluginsOnDisk();
  plugins.erase(std::remove_if(plugins.begin(), plugins.end(),
                               [](const Entry& e) {
                                 return !isDiscoverableEntry(isPluginQuarantined(e.dir), e.hasPluginJs, e.hasDevice,
                                                             e.hasManifest);
                               }),
                plugins.end());
  return plugins;
}
bool anyPluginInstalledOnDisk() { return !scanPluginsOnDisk().empty(); }
std::string findPluginDir(const char* name) {
  if (!SETTINGS.pluginsEnabled) return {};
  auto dir = findPluginDirOnDisk(name);
  return dir.empty() || isPluginQuarantined(dir) ? std::string{} : dir;
}
}  // namespace PluginLocations
