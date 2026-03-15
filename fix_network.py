import os

mock_dir = "lib/simulator_mock/src"
def w(name, content):
    with open(os.path.join(mock_dir, name), "w") as f:
        f.write(content)

w("WiFi.h", """#pragma once
class WiFiClass {
public:
    int begin(const char*, const char*) { return 0; }
    int status() { return 3; } // WL_CONNECTED
    const char* localIP() { return "127.0.0.1"; }
    void disconnect() {}
    void mode(int) {}
    void softAP(const char*, const char*) {}
    const char* softAPIP() { return "192.168.4.1"; }
};
extern WiFiClass WiFi;
#define WIFI_STA 1
#define WIFI_AP 2
#define WL_CONNECTED 3
""")

w("ESPmDNS.h", """#pragma once
class MDNSClass {
public:
    bool begin(const char*) { return true; }
    void end() {}
    void addService(const char*, const char*, int) {}
};
extern MDNSClass MDNS;
""")

w("DNSServer.h", """#pragma once
class DNSServer {
public:
    void start(int, const char*, const char*) {}
    void processNextRequest() {}
};
""")

w("esp_system.h", """#pragma once
inline void esp_restart() {}
""")

w("Stream.h", """#pragma once
#include "Print.h"
#include "WString.h"
class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() override {}
    String readStringUntil(char terminator) { return String(""); }
    size_t readBytes(char *buffer, size_t length) { return 0; }
};
""")

with open(os.path.join(mock_dir, "HardwareSerial.h"), "r") as f:
    hs = f.read()
if "class HWCDC : public Stream" not in hs:
    hs = hs.replace("class HWCDC : public Print", '#include "Stream.h"\nclass HWCDC : public Stream')
    hs = hs.replace("int available() {", "int available() override {")
    hs = hs.replace("String readStringUntil(", "//")
    with open(os.path.join(mock_dir, "HardwareSerial.h"), "w") as f:
        f.write(hs)

# Replace macros with templates
arduino_path = os.path.join(mock_dir, "Arduino.h")
with open(arduino_path, "r") as f:
    ard = f.read()
ard = ard.replace("#ifndef max\\n#define max(a,b) ((a)>(b)?(a):(b))\\n#endif", "")
ard = ard.replace("#ifndef min\\n#define min(a,b) ((a)<(b)?(a):(b))\\n#endif", "")
# if we have not added templates, add them
if "constexpr auto max(A a, B b)" not in ard:
    ard += """
template <typename A, typename B> constexpr auto max(A a, B b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <typename A, typename B> constexpr auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
"""
with open(arduino_path, "w") as f:
    f.write(ard)
