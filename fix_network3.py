import os

mock_dir = "lib/simulator_mock/src"
def w(name, content):
    path = os.path.join(mock_dir, name)
    with open(path, "w") as f:
        f.write(content)

# Update WiFi.h to add SSID()
wifi_path = os.path.join(mock_dir, "WiFi.h")
with open(wifi_path, "r") as f:
    wifi = f.read()

if "String SSID() {" not in wifi:
    wifi = wifi.replace("String SSID(int i) {", "String SSID() { return String(\"MockWiFi\"); }\\n    String SSID(int i) {")
with open(wifi_path, "w") as f:
    f.write(wifi)

# Create esp_sntp.h
w("esp_sntp.h", """#pragma once
inline void sntp_setoperatingmode(int) {}
inline void sntp_setservername(int, const char*) {}
inline void sntp_init() {}
inline void sntp_stop() {}
inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval *)) {}
#define SNTP_OPMODE_POLL 0
""")

# Fix esp_system.h
sys_path = os.path.join(mock_dir, "esp_system.h")
with open(sys_path, "r") as f:
    sys = f.read()
if "esp_get_free_heap_size" not in sys:
    sys += "\\ninline uint32_t esp_get_free_heap_size() { return 1000000; }\\n"
    with open(sys_path, "w") as f:
        f.write(sys)

# Fix Print.h class correctly
print_path = os.path.join(mock_dir, "Print.h")
with open(print_path, "w") as f:
    f.write("""#pragma once
#include <cstdint>
#include <cstddef>
class Print {
public:
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t n = 0;
        while(size--) {
            n += write(*buffer++);
        }
        return n;
    }
    virtual void flush() {}
    
    // Add missing overloads from Print
    size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
    size_t println(const char* s) { size_t n = print(s); n += print("\\r\\n"); return n; }
    size_t println(int n) { return println("1"); }
};
""")

print("Done")
