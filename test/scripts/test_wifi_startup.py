#!/usr/bin/env python3
"""Exercise the production connection setup with a stopped-radio Wi-Fi fake."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/activities/network/WifiSelectionActivity.cpp").read_text()
start = source.index("void WifiSelectionActivity::attemptConnection()")
end = source.index("\n}", start) + 2
method = source[start:end]

harness = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

template <typename... Args> void log(const char*, Args...) {}
#define LOG_INF(...) log(__VA_ARGS__)
#define LOG_ERR(...) log(__VA_ARGS__)
#define LOG_DBG(...) log(__VA_ARGS__)

enum wl_status_t { WL_IDLE_STATUS, WL_DISCONNECTED };
enum class WifiSelectionState { CONNECTING, AUTO_CONNECTING, CONNECTION_FAILED };
constexpr int WIFI_STA = 1, WIFI_ALL_CHANNEL_SCAN = 2, WIFI_CONNECT_AP_BY_SIGNAL = 3;
constexpr int ESP_MAC_WIFI_STA = 0, ESP_OK = 0, STR_ERROR_GENERAL_FAILURE = 1;
uint8_t sLastStaDisconnectReason = 2;
bool sConnectionAttemptLoggingActive = true;
std::vector<std::string> calls;
bool macReadOk = true;

unsigned long millis() { return 1000; }
void delay(unsigned ms) { assert(ms == 100); calls.emplace_back("delay"); }
const char* tr(int) { return "General failure"; }
const char* wifiStatusName(wl_status_t) { return "DISCONNECTED"; }
struct { unsigned getFreeHeap() { return 90000; } unsigned getMaxAllocHeap() { return 49000; } } ESP;
struct {
  bool modeOk = true, resetOk = true, running = false;
  std::string hostname, ssid, password;
  void persistent(bool enabled) { assert(!enabled); calls.emplace_back("persistent"); }
  bool mode(int mode) {
    assert(mode == WIFI_STA);
    calls.emplace_back("station");
    running = modeOk;
    return modeOk;
  }
  bool disconnect(bool off, bool erase) {
    assert(off && erase);
    assert(!sConnectionAttemptLoggingActive);
    calls.emplace_back("reset");
    running = false;
    return resetOk;
  }
  void setScanMethod(int method) { assert(method == WIFI_ALL_CHANNEL_SCAN); calls.emplace_back("scan"); }
  void setSortMethod(int method) { assert(method == WIFI_CONNECT_AP_BY_SIGNAL); calls.emplace_back("sort"); }
  void setHostname(const char* value) {
    assert(!running);
    hostname = value;
    calls.emplace_back("hostname");
  }
  wl_status_t begin(const char* name, const char* pass = nullptr) {
    assert(sConnectionAttemptLoggingActive && sLastStaDisconnectReason == 0);
    ssid = name;
    password = pass ? pass : "";
    calls.emplace_back("begin");
    running = true;
    return WL_DISCONNECTED;
  }
} WiFi;

int esp_read_mac(uint8_t* mac, int type) {
  assert(type == ESP_MAC_WIFI_STA && !WiFi.running);
  calls.emplace_back("mac");
  for (unsigned i = 0; i < 6; ++i) mac[i] = 0xA0 + i;
  return macReadOk ? ESP_OK : -1;
}

struct WifiSelectionActivity {
  bool autoConnecting = false, usedSavedPassword = false, selectedRequiresPassword = true;
  bool autoFailureHandled = false;
  unsigned long connectionStartTime = 0, lastConnectionStatusLogTime = 0;
  int lastLoggedWifiStatus = 0;
  WifiSelectionState state = WifiSelectionState::CONNECTION_FAILED;
  std::string selectedSSID = "test network", enteredPassword = " A!@#$%^&*() z ";
  std::string connectedIP = "old", connectionError = "old";
  void requestUpdate() {}
  void handleAutoConnectFailure() { autoFailureHandled = true; }
  void attemptConnection();
};
'''

cases = r'''
int main() {
  for (const bool automatic : {false, true}) {
    for (const bool encrypted : {false, true}) {
      calls.clear();
      WifiSelectionActivity activity;
      activity.autoConnecting = automatic;
      activity.selectedRequiresPassword = encrypted;
      activity.attemptConnection();
      assert((calls == std::vector<std::string>{"persistent", "station", "reset", "delay",
                                               "scan", "sort", "mac", "hostname", "begin"}));
      assert(WiFi.hostname == "CrossPoint-Reader-A0A1A2A3A4A5");
      assert(WiFi.ssid == activity.selectedSSID);
      assert(WiFi.password == (encrypted ? activity.enteredPassword : ""));
      assert(activity.connectedIP.empty() && activity.connectionError.empty());
      assert(activity.state == (automatic ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING));
    }
  }

  // A mode failure must not start a connection or skip automatic fallback.
  WiFi.modeOk = false;
  for (const bool automatic : {false, true}) {
    calls.clear();
    WifiSelectionActivity activity;
    activity.autoConnecting = automatic;
    activity.attemptConnection();
    assert((calls == std::vector<std::string>{"persistent", "station"}));
    assert(activity.connectionError == "General failure");
    assert(!sConnectionAttemptLoggingActive);
    assert(activity.autoFailureHandled == automatic);
    if (!automatic) assert(activity.state == WifiSelectionState::CONNECTION_FAILED);
  }

  // Like upstream, reset failure still permits an explicit begin().
  WiFi.modeOk = true;
  WiFi.resetOk = false;
  calls.clear();
  WifiSelectionActivity resetFailure;
  resetFailure.attemptConnection();
  assert(calls.back() == "begin");

  // An unavailable hardware MAC must not prevent the connection.
  WiFi.resetOk = true;
  macReadOk = false;
  WiFi.hostname.clear();
  WifiSelectionActivity macFailure;
  macFailure.attemptConnection();
  assert(WiFi.hostname.empty() && WiFi.ssid == macFailure.selectedSSID);
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-wifi-startup-") as tmp:
    root = Path(tmp)
    test_source = root / "test.cpp"
    test_source.write_text(harness + method + cases)
    executable = root / "test"
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", str(test_source), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)

print("PASS: upstream reset ordering, stopped-radio hostname, raw credentials, open/manual/auto and failure paths")
