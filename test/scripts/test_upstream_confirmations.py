#!/usr/bin/env python3
"""Execute production confirmation handlers with UI/storage boundary doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#define LOG_ERR(...) ((void)0)
struct RenderLock {};
struct ActivityResult { bool isCancelled; };
enum class StrId { STR_CANCEL, STR_PLUGIN_UNINSTALL };
constexpr auto STR_PLUGIN_UNINSTALL_CONFIRM = "Uninstall %s and delete all of its files?";
constexpr auto STR_PLUGIN_UNINSTALL_TITLE = "Uninstall plugin";
constexpr auto STR_SET_SLEEP_COVER_CONFIRM = "Set sleep cover?";
constexpr auto STR_PIN_PNG_WARNING = "Switch to Page Overlay";
const char* tr(const char* value) { return value; }
struct MappedInputManager {
  enum class Button { Confirm };
  bool pressed = false, released = false;
  int held = 0;
  bool isPressed(Button) const { return pressed; }
  bool wasReleased(Button) const { return released; }
  int getHeldTime() const { return held; }
};
struct ConfirmationActivity {
  bool ignoreRelease;
  std::string body;
  ConfirmationActivity(int&, MappedInputManager&, const std::string&, const std::string& text, bool ignore = false)
    : ignoreRelease(ignore), body(text) {}
  void setOptionLabels(StrId, StrId) {}
};
bool failAllocation = false;
template<class T, class... Args> auto makeUniqueNoThrow(Args&&... args) {
  return failAllocation ? std::unique_ptr<T>{} : std::make_unique<T>(std::forward<Args>(args)...);
}
struct UiListActivity { int forwarded = 0; bool handleButtons() { ++forwarded; return false; } };
namespace PluginLocations { constexpr const char* ROOTS[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"}; }
struct StorageDouble {
  std::set<std::string> folders;
  std::vector<std::string> removed;
  bool failRemove = false;
  bool exists(const char* path) { return folders.contains(path); }
  bool removeDir(const char* path) { removed.push_back(path); return !failRemove && folders.erase(path); }
} Storage;
struct PluginCatalogActivity : UiListActivity {
  enum class State { PLUGIN_PICKER, BROWSING } state = State::PLUGIN_PICKER;
  struct PluginRef { std::string name, title, dir; };
  std::vector<PluginRef> installedPlugins;
  bool showPluginHubInstallRow = true, uninstallHoldHandled = false;
  int renderer = 0, refreshes = 0, prompts = 0;
  MappedInputManager mappedInput;
  struct { void clearTapFlash() {} } app;
  struct Nav { int selected = 0; } nav;
  std::string pickerReturnName;
  std::function<void(const ActivityResult&)> callback;
  std::unique_ptr<ConfirmationActivity> confirmation;
  Nav& activeNav() { return nav; }
  void requestUpdate() { ++refreshes; }
  void enterPluginPicker() { ++refreshes; }
  void startActivityForResult(std::unique_ptr<ConfirmationActivity> value, std::function<void(const ActivityResult&)> done) {
    ++prompts; confirmation = std::move(value); callback = std::move(done);
  }
  void confirmUninstallPlugin(int);
  void onRowLongPress(int);
  bool handleButtons();
};
constexpr int PLUGIN_UNINSTALL_HOLD_MS = 1000;
struct CrossPointSettings { enum { OVERLAY = 6, COVER_OVERLAY = 12 }; int sleepScreen = 3; } SETTINGS;
namespace FsHelpers { bool hasPngExtension(const std::string& name) { return name.ends_with(".png"); } }
std::string imageDisplayName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }
struct BmpViewerActivity {
  int renderer = 0, saves = 0, redraws = 0;
  MappedInputManager mappedInput;
  std::string filePath = "/sleep.png";
  std::atomic<bool> needsImageRedraw{false};
  std::unique_ptr<ConfirmationActivity> confirmation;
  std::function<void(const ActivityResult&)> callback;
  void startActivityForResult(std::unique_ptr<ConfirmationActivity> value, std::function<void(const ActivityResult&)> done) {
    confirmation = std::move(value); callback = std::move(done);
  }
  void doSetSleepCover() { ++saves; }
  void requestImageRedraw() { ++redraws; }
  void confirmSetSleepCover();
};
'''
main = r'''
int main() {
  PluginCatalogActivity activity;
  activity.installedPlugins = {{"same", "First copy", "/plugins/same"}, {"next", "Next plugin", "/.plugins/next"}};
  Storage.folders = {"/plugins/same", "/.crosspoint/plugins/same", "/.plugins/next"};
  activity.confirmUninstallPlugin(0);
  assert(activity.prompts == 0); // Hub install row
  activity.onRowLongPress(1);
  assert(activity.prompts == 1 && Storage.removed.empty());
  activity.callback({true});
  assert(Storage.removed.empty());
  activity.confirmUninstallPlugin(1);
  activity.callback({false});
  assert(Storage.removed == std::vector<std::string>{"/plugins/same"});
  assert(Storage.folders.contains("/.crosspoint/plugins/same"));
  assert(activity.pickerReturnName == "next");
  for (const auto name : {"..", "a/b", "a\\b", ""}) {
    activity.installedPlugins[0] = {name, "Unsafe", std::string("/plugins/") + name};
    const auto before = activity.prompts;
    activity.confirmUninstallPlugin(1);
    assert(activity.prompts == before);
  }
  activity.installedPlugins[0] = {"same", "Wrong root", "/plugins-other/same"};
  auto before = activity.prompts;
  activity.confirmUninstallPlugin(1);
  assert(activity.prompts == before);
  activity.installedPlugins[0] = {"same", "First copy", "/plugins/same"};
  activity.nav.selected = 1;
  activity.mappedInput.pressed = true;
  activity.mappedInput.held = 999;
  activity.handleButtons();
  assert(activity.prompts == before);
  activity.mappedInput.held = 1000;
  assert(activity.handleButtons());
  assert(activity.prompts == before + 1 && activity.confirmation->ignoreRelease);
  activity.handleButtons();
  assert(activity.prompts == before + 1);
  activity.mappedInput.pressed = false;
  activity.mappedInput.released = true;
  assert(activity.handleButtons()); // originating release does not activate a row
  failAllocation = true;
  before = activity.prompts;
  activity.confirmUninstallPlugin(1);
  assert(activity.prompts == before);
  failAllocation = false;
  BmpViewerActivity image;
  image.confirmSetSleepCover();
  assert(image.saves == 0 && image.confirmation->body.find("Switch") != std::string::npos);
  image.callback({true});
  assert(image.saves == 0 && image.redraws == 1);
  image.confirmSetSleepCover();
  image.callback({false});
  assert(image.saves == 1);
  SETTINGS.sleepScreen = CrossPointSettings::COVER_OVERLAY;
  image.confirmSetSleepCover();
  assert(image.confirmation->body == "sleep.png");
  std::puts("Plugin uninstall paths, cancellation, hold latch and image confirmation passed");
}
'''
plugin = 'src/activities/plugins/PluginCatalogActivity.cpp'
code = prefix + '\n'.join(function(plugin, signature) for signature in (
    'void PluginCatalogActivity::confirmUninstallPlugin(',
    'void PluginCatalogActivity::onRowLongPress(',
    'bool PluginCatalogActivity::handleButtons(',
)) + function('src/activities/util/BmpViewerActivity.cpp', 'void BmpViewerActivity::confirmSetSleepCover(') + main
with tempfile.TemporaryDirectory(prefix='crossink-confirmations-') as temp:
    source, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
    source.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-fsanitize=address,undefined',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
