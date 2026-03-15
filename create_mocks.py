import os

MOCK_DIR = "lib/simulator_mock/src"
os.makedirs(os.path.join(MOCK_DIR, "freertos"), exist_ok=True)

files = {
    "Arduino.h": """#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>

#define PROGMEM
#define ICACHE_RODATA_ATTR
#define PGM_P const char *
#define PSTR(s) (s)

#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

inline unsigned long millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline void delay(unsigned long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline void yield() {
    std::this_thread::yield();
}

#include "WString.h"
#include "Print.h"
#include "HardwareSerial.h"

struct ESPMock {
    uint32_t getFreeHeap() { return 1024*1024; }
    uint32_t getHeapSize() { return 1024*1024; }
    uint32_t getMinFreeHeap() { return 1024*1024; }
    uint32_t getMaxAllocHeap() { return 1024*1024; }
};
extern ESPMock ESP;
""",

    "WString.h": """#pragma once
#include <string>
class String : public std::string {
public:
    String() : std::string() {}
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    bool startsWith(const String& prefix) const {
        return this->find(prefix) == 0;
    }
    String substring(unsigned int from, unsigned int to = (unsigned int)-1) const {
        if (from >= this->length()) return String();
        return String(this->substr(from, to - from));
    }
    void trim() {
        size_t first = find_first_not_of(" \t\n\r");
        if(first == std::string::npos) { clear(); return; }
        size_t last = find_last_not_of(" \t\n\r");
        *this = substr(first, (last-first+1));
    }
};
""",

    "Print.h": """#pragma once
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
};
""",

    "HardwareSerial.h": """#pragma once
#include "Print.h"
#include "WString.h"
#include <iostream>

class HWCDC : public Print {
public:
    void begin(unsigned long baud) {}
    size_t write(uint8_t c) override { std::cout << (char)c; return 1; }
    size_t write(const uint8_t *buffer, size_t size) override {
        std::cout.write((const char*)buffer, size);
        return size;
    }
    int available() { return 0; }
    String readStringUntil(char terminator) { return String(""); }
    template<typename... Args>
    void printf(const char* format, Args... args) {
        char buf[256];
        snprintf(buf, sizeof(buf), format, args...);
        std::cout << buf;
    }
    operator bool() const { return true; }
};

extern HWCDC Serial;
""",

    "SPI.h": """#pragma once
class SPIClass {
public:
    void begin(int sck=-1, int miso=-1, int mosi=-1, int ss=-1) {}
    void end() {}
};
extern SPIClass SPI;
""",

    "FS.h": """#pragma once
#include <cstdint>
#include <string>

class File {
public:
    operator bool() const { return true; }
    void close() {}
    size_t write(const uint8_t *buf, size_t size) { return size; }
    int read() { return -1; }
    size_t read(uint8_t* buf, size_t size) { return 0; }
    uint32_t position() const { return 0; }
    void seek(uint32_t pos) {}
    uint32_t size() const { return 0; }
};

class FS {
public:
    bool begin() { return true; }
    File open(const char* path, const char* mode = "r") { return File(); }
    bool exists(const char* path) { return false; }
    bool remove(const char* path) { return true; }
    bool rename(const char* pathFrom, const char* pathTo) { return true; }
    bool mkdir(const char* path) { return true; }
    bool rmdir(const char* path) { return true; }
};
""",

    "SD.h": """#pragma once
#include "FS.h"
extern FS SD;
""",

    "freertos/FreeRTOS.h": """#pragma once
""",

    "freertos/task.h": """#pragma once
""",

    "EInkDisplay.h": """#pragma once
#include <cstdint>
#define EPD_SCLK 0
#define EPD_MOSI 0
#define EPD_CS 0
#define EPD_DC 0
#define EPD_RST 0
#define EPD_BUSY 0

class EInkDisplay {
public:
    static constexpr uint16_t DISPLAY_WIDTH = 1200;
    static constexpr uint16_t DISPLAY_HEIGHT = 825;
    
    enum RefreshMode {
        FULL_REFRESH,
        HALF_REFRESH,
        FAST_REFRESH
    };

    EInkDisplay(int, int, int, int, int, int) {}
    void begin() {}
    void clearScreen(uint8_t color) {}
    void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) {}
    void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) {}
    void displayBuffer(RefreshMode mode, bool turnOffScreen) {}
    void refreshDisplay(RefreshMode mode, bool turnOffScreen) {}
    void deepSleep() {}
    uint8_t* getFrameBuffer() { static uint8_t buf[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8]; return buf; }
    void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {}
    void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {}
    void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {}
    void cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {}
    void displayGrayBuffer(bool turnOffScreen = false) {}
};
""",

    "HalStorage.h": """#pragma once
#include "FS.h"
class HalStorage {
public:
    bool begin() { return true; }
    bool begin(void* spi) { return true; }
};
extern HalStorage Storage;
""",
}

for name, content in files.items():
    with open(os.path.join(MOCK_DIR, name), "w") as f:
        f.write(content)

with open(os.path.join(MOCK_DIR, "ESP.cpp"), "w") as f:
    f.write('''#include "Arduino.h"
ESPMock ESP;
HWCDC Serial;
SPIClass SPI;
FS SD;
HalStorage Storage;
''')

print("Created mocks.")
