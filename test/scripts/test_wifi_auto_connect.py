#!/usr/bin/env python3
"""Exercise production saved-network retry methods with fake Wi-Fi and scans.

This checks retry/fallback decisions, not radio, DHCP, or physical input timing.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/activities/network/WifiSelectionActivity.cpp").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end] + "\n"


methods = "\n".join(function(signature) for signature in (
    "bool WifiSelectionActivity::hasAttemptedAutoSsid(",
    "bool WifiSelectionActivity::tryAutoConnectCredential(",
    "bool WifiSelectionActivity::tryNextSavedNetworkFromScan()",
    "void WifiSelectionActivity::handleAutoConnectFailure()",
    "void WifiSelectionActivity::showNetworkListFromAutoConnect()",
))

harness = r'''
#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <vector>
#define LOG_DBG(...) ((void)0)

struct WifiCredential { std::string ssid, password; };
struct WifiNetworkInfo { std::string ssid; bool hasSavedPassword; };
struct {
  std::vector<WifiCredential> credentials{{"home", "password"}, {"backup", "password"}};
  std::optional<WifiCredential> findCredential(const std::string& ssid) const {
    for (const auto& credential : credentials)
      if (credential.ssid == ssid) return credential;
    return std::nullopt;
  }
} WIFI_STORE;
struct { void disconnect() {} } WiFi;
enum class WifiSelectionState { AUTO_CONNECTING, SCANNING, NETWORK_LIST };
struct WifiSelectionActivity {
  std::vector<std::string> autoAttemptedSsids, attempts;
  std::vector<WifiNetworkInfo> networks;
  std::string selectedSSID, enteredPassword;
  bool selectedRequiresPassword = false, usedSavedPassword = false;
  bool autoConnecting = false, manualNetworkListRequested = false;
  bool radioReleased = false;
  size_t selectedNetworkIndex = 0;
  unsigned scans = 0;
  WifiSelectionState state = WifiSelectionState::SCANNING;
  void requestUpdate() {}
  void releaseWifiForNetworkList() { radioReleased = true; }
  void attemptConnection() {
    attempts.push_back(selectedSSID);
    state = WifiSelectionState::AUTO_CONNECTING;
  }
  void startWifiScan(bool automatic) {
    ++scans;
    autoConnecting = automatic;
    manualNetworkListRequested = false;
    networks.clear();
    state = WifiSelectionState::SCANNING;
  }
  bool hasAttemptedAutoSsid(const std::string&) const;
  bool tryAutoConnectCredential(const WifiCredential&);
  bool tryNextSavedNetworkFromScan();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
};
'''

cases = r'''
int main() {
  const auto home = *WIFI_STORE.findCredential("home");

  // A failed quick connection may succeed after scanning the same saved AP.
  WifiSelectionActivity retry;
  assert(retry.tryAutoConnectCredential(home));
  retry.handleAutoConnectFailure();
  assert(retry.scans == 1 && retry.autoConnecting);
  retry.networks = {{"home", true}, {"backup", true}, {"Add hidden network", false}};
  assert(retry.tryNextSavedNetworkFromScan());
  assert((retry.attempts == std::vector<std::string>{"home", "home"}));
  assert(retry.usedSavedPassword && retry.enteredPassword == home.password);

  // Repeated failure moves to another saved network, then stops at the picker.
  retry.handleAutoConnectFailure();
  assert((retry.attempts == std::vector<std::string>{"home", "home", "backup"}));
  retry.handleAutoConnectFailure();
  assert(retry.state == WifiSelectionState::NETWORK_LIST);
  assert(!retry.autoConnecting && retry.radioReleased && retry.scans == 1);
  assert(!retry.tryNextSavedNetworkFromScan());

  // An absent last-used AP must not be retried from stale credentials.
  WifiSelectionActivity absent;
  assert(absent.tryAutoConnectCredential(home));
  absent.handleAutoConnectFailure();
  absent.networks = {{"guest", false}, {"backup", true}, {"Add hidden network", false}};
  assert(absent.tryNextSavedNetworkFromScan());
  assert((absent.attempts == std::vector<std::string>{"home", "backup"}));
  absent.handleAutoConnectFailure();
  assert(absent.state == WifiSelectionState::NETWORK_LIST && absent.scans == 1);

  // Starting with scan results (no last-used AP) still tries each saved AP once.
  WifiSelectionActivity scanFirst;
  scanFirst.networks = {{"home", true}, {"Add hidden network", false}};
  assert(scanFirst.tryNextSavedNetworkFromScan());
  scanFirst.handleAutoConnectFailure();
  assert(scanFirst.state == WifiSelectionState::NETWORK_LIST && scanFirst.scans == 0);
  assert(scanFirst.attempts.size() == 1);

  // No visible saved networks must not initiate a connection.
  WifiSelectionActivity noMatch;
  noMatch.networks = {{"guest", false}, {"Add hidden network", false}};
  assert(!noMatch.tryNextSavedNetworkFromScan());
  assert(noMatch.attempts.empty());

  // The explicit picker action still interrupts the automatic flow.
  WifiSelectionActivity manual;
  assert(manual.tryAutoConnectCredential(home));
  manual.showNetworkListFromAutoConnect();
  assert(!manual.autoConnecting && manual.scans == 1);
  manual.networks = {{"home", true}, {"Add hidden network", false}};
  manual.showNetworkListFromAutoConnect();
  assert(manual.state == WifiSelectionState::NETWORK_LIST);
  assert(manual.manualNetworkListRequested && manual.radioReleased);
  assert(manual.attempts.size() == 1);
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-wifi-auto-") as tmp:
    root = Path(tmp)
    test_source = root / "test.cpp"
    test_source.write_text(harness + methods + cases)
    executable = root / "test"
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", str(test_source), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)

print("PASS: saved Wi-Fi retry, bounded fallback, absent AP, scan-first, no match, manual picker")
