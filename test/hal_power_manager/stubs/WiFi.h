#pragma once
using wifi_mode_t = int;
constexpr int WIFI_MODE_NULL = 0, WIFI_MODE_AP = 1, WIFI_MODE_STA = 2, WIFI_OFF = 0;
struct FakeWiFi {
  int value = WIFI_MODE_NULL;
  int getMode() const { return value; }
  void mode(int mode) { value = mode; }
  void softAPdisconnect(bool) {}
  void disconnect(bool) {}
};
inline FakeWiFi WiFi;
