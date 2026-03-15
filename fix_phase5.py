import os

mock_dir = "lib/simulator_mock/src"
def w(name, content):
    with open(os.path.join(mock_dir, name), "w") as f:
        f.write(content)

w("HTTPClient.h", """#pragma once
#include "WString.h"
#include <WiFi.h>
class HTTPClient {
public:
    bool begin(const char*) { return true; }
    int GET() { return 200; }
    String getString() { return String(""); }
    void end() {}
};
""")

w("esp_http_client.h", """#pragma once
typedef void* esp_http_client_handle_t;
typedef struct { const char* url; } esp_http_client_config_t;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*) { return nullptr; }
inline int esp_http_client_perform(esp_http_client_handle_t) { return 0; }
inline void esp_http_client_cleanup(esp_http_client_handle_t) {}
""")

# Edit WString.h for unsigned long constructor
wstring_path = os.path.join(mock_dir, "WString.h")
with open(wstring_path, "r") as f:
    ws = f.read()
if "String(unsigned long val)" not in ws:
    ws = ws.replace("String(const std::string& str) : s(str) {}", "String(const std::string& str) : s(str) {}\n    String(unsigned long val) { s = std::to_string(val); }")
with open(wstring_path, "w") as f:
    f.write(ws)

# Edit WebServer.h to add RequestHandler
server_path = os.path.join(mock_dir, "WebServer.h")
with open(server_path, "r") as f:
    server = f.read()

if "class RequestHandler" not in server:
    server += """
class WebServer;
class RequestHandler {
public:
    virtual ~RequestHandler() {}
    virtual bool canHandle(WebServer& request, int method, String uri) { return false; }
    virtual bool canUpload(WebServer& request, String uri) { return false; }
    virtual bool handle(WebServer& request, int method, String uri) { return false; }
    virtual void upload(WebServer& request, String uri) {}
};

enum HTTPRaw { RAW_START };

class WebServerExt : public WebServer {
public:
    WebServerExt(int port) : WebServer(port) {}
    void addHandler(RequestHandler* handler) {}
};
#define WebServer WebServerExt
"""
    with open(server_path, "w") as f:
        f.write(server)

# Edit platformio.ini to add QRCode and -Wno-c++11-narrowing
pio_path = "platformio.ini"
with open(pio_path, "r") as f:
    pio = f.read()

if "-Wno-c++11-narrowing" not in pio:
    pio = pio.replace("-D SIMULATOR", "-D SIMULATOR\n  -Wno-c++11-narrowing")
if "ricmoo/QRCode" not in pio.split("[env:simulator]")[1]:
    pio = pio.replace("simulator_mock=symlink://lib/simulator_mock", "simulator_mock=symlink://lib/simulator_mock\n  ricmoo/QRCode @ ^0.0.1")

with open(pio_path, "w") as f:
    f.write(pio)

print("Done phase 5")
