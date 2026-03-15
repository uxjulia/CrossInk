#pragma once
#include "NetworkClient.h"
#include "WString.h"

// On ESP32, WiFiClient is a TCP client backed by the WiFi stack.
// In the simulator NetworkClient already provides the same interface.
using WiFiClient = NetworkClient;

typedef int wifi_mode_t;

#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
#define WL_CONNECTED 3

class IPAddress {
  uint8_t bytes[4] = {127, 0, 0, 1};

 public:
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
  }
  String toString() const { return String("127.0.0.1"); }
  uint8_t operator[](int i) const { return bytes[i % 4]; }
  uint8_t& operator[](int i) { return bytes[i % 4]; }
  bool operator==(const IPAddress& o) const { return true; }
  bool operator!=(const IPAddress& o) const { return false; }
};

class WiFiClass {
 public:
  int begin(const char* ssid = nullptr, const char* pass = nullptr) { return 3; }
  int status() { return 3; }  // WL_CONNECTED
  IPAddress localIP() { return IPAddress(); }
  void disconnect(bool wifioff = false) {}
  void mode(int) {}
  bool softAP(const char* ssid, const char* pass = NULL, int channel = 1, int hidden = 0, int max_connection = 4) {
    return true;
  }
  bool softAPdisconnect(bool wifioff = false) { return true; }
  IPAddress softAPIP() { return IPAddress(); }

  String macAddress() { return String("00:00:00:00:00:00"); }
  uint8_t* macAddress(uint8_t* mac) { return mac; }

  void scanDelete() {}
  int scanNetworks(bool async = false, bool show_hidden = false, bool passive = false, uint32_t max_ms_per_chan = 300,
                   uint8_t channel = 0) {
    return 0;
  }
  int scanComplete() { return -1; }
  String SSID() { return String("MockWiFi"); }
  String SSID(int i) { return String("MockWiFi"); }
  int RSSI() { return -50; }
  int RSSI(int i) { return -50; }
  int encryptionType(int i) { return 0; }
  void setHostname(const char*) {}
  wifi_mode_t getMode() { return WIFI_MODE_STA; }
  void setSleep(bool) {}
  String getHostname() { return String("mock-hostname"); }
  int softAPgetStationNum() { return 0; }
};
extern WiFiClass WiFi;

#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_OFF 0
#define WL_CONNECTED 3
#define WL_CONNECT_FAILED 4
#define WL_NO_SSID_AVAIL 1
#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2
#define WIFI_AUTH_OPEN 0
typedef int wl_status_t;
