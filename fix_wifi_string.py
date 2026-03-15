import os

mock_dir = "lib/simulator_mock/src"
def w(name, content):
    with open(os.path.join(mock_dir, name), "w") as f:
        f.write(content)

# Update WiFi.h
w("WiFi.h", """#pragma once
#include "WString.h"

class IPAddress {
public:
    IPAddress() {}
    IPAddress(int,int,int,int) {}
    String toString() const { return String("127.0.0.1"); }
};

class WiFiClass {
public:
    int begin(const char* ssid = nullptr, const char* pass = nullptr) { return 3; }
    int status() { return 3; } // WL_CONNECTED
    IPAddress localIP() { return IPAddress(); }
    void disconnect() {}
    void mode(int) {}
    void softAP(const char*, const char*) {}
    IPAddress softAPIP() { return IPAddress(); }
    String macAddress() { return String("00:00:00:00:00:00"); }
    void scanDelete() {}
    int scanNetworks() { return 0; }
    int scanComplete() { return -1; }
    String SSID(int i) { return String("MockWiFi"); }
    int RSSI(int i) { return -50; }
    int encryptionType(int i) { return 0; }
    void setHostname(const char*) {}
};
extern WiFiClass WiFi;
#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_OFF 0
#define WL_CONNECTED 3
#define WL_CONNECT_FAILED 4
#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2
#define WIFI_AUTH_OPEN 0
typedef int wl_status_t;
""")

# esp_task_wdt.h
w("esp_task_wdt.h", """#pragma once
inline void esp_task_wdt_reset() {}
inline void esp_task_wdt_init(int, bool) {}
inline void esp_task_wdt_add(void*) {}
""")

# Edit WString.h for `replace` and `operator+`
wstring_path = os.path.join(mock_dir, "WString.h")
with open(wstring_path, "r") as f:
    ws = f.read()

if "void replace(" not in ws:
    ws = ws.replace("bool isEmpty() const", "void replace(const char*, const char*) {}\n    bool isEmpty() const")
if "inline String operator+(" not in ws:
    ws += """
inline String operator+(const char* lhs, const String& rhs) {
    String res(lhs);
    res += rhs.c_str();
    return res;
}
"""
with open(wstring_path, "w") as f:
    f.write(ws)

# Edit Print.h for virtual flush
print_path = os.path.join(mock_dir, "Print.h")
with open(print_path, "r") as f:
    pr = f.read()
if "virtual void flush" not in pr:
    pr = pr.replace("void flush()", "virtual void flush()")
with open(print_path, "w") as f:
    f.write(pr)

# Ensure Stream.h exists and works
stream_path = os.path.join(mock_dir, "Stream.h")
if os.path.exists(stream_path):
    with open(stream_path, "r") as f:
        stream = f.read()
    if "virtual void flush() override" in stream:
        pass # it's fine
print("Fixed WiFi, esp_task_wdt, WString, and Print")
