import os

MOCK_DIR = "lib/simulator_mock/src"
os.makedirs(MOCK_DIR, exist_ok=True)

files = {
    # ------------------- HalDisplay.cpp -------------------
    "HalDisplay.cpp": """
#include "HalDisplay.h"
#include <SDL2/SDL.h>
#include <iostream>

static SDL_Window* window = nullptr;
static SDL_Renderer* sdl_renderer = nullptr;
static SDL_Texture* texture = nullptr;
static uint32_t pixels[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }
    window = SDL_CreateWindow("Simulator - Open-X4 SDK",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, SDL_WINDOW_SHOWN);
    sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    // Initialize white
    for (int i=0; i<DISPLAY_WIDTH*DISPLAY_HEIGHT; i++) pixels[i] = 0xFFFFFFFF;
}

void HalDisplay::clearScreen(uint8_t color) const {
    uint32_t c = (color == 0xFF) ? 0xFFFFFFFF : 0xFF000000;
    for (int i=0; i<DISPLAY_WIDTH*DISPLAY_HEIGHT; i++) pixels[i] = c;
}

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem) const {}
void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem) const {}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
    refreshDisplay(mode, turnOffScreen);
}

void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) {
    if (!texture || !sdl_renderer) return;

    static uint8_t* fb = getFrameBuffer();
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            int byteIdx = (y * DISPLAY_WIDTH + x) / 8;
            int bitIdx = 7 - (x % 8);
            bool isWhite = (fb[byteIdx] & (1 << bitIdx)) != 0;
            // The display might be inverse or standard. Here let's assume 1=white, 0=black.
            // Actually usually 1=white in epd.
            pixels[y * DISPLAY_WIDTH + x] = isWhite ? 0xFFFFFFFF : 0xFF000000;
        }
    }
    SDL_UpdateTexture(texture, nullptr, pixels, DISPLAY_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(sdl_renderer);
    
    // Process SDL events here as a hack, since display is refreshed often
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) exit(0);
    }
}

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const {
    static uint8_t buffer[HalDisplay::BUFFER_SIZE];
    return buffer;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {}
void HalDisplay::displayGrayBuffer(bool turnOffScreen) {}
""",

    # ------------------- HalGPIO.cpp -------------------
    "HalGPIO.cpp": """
#include "HalGPIO.h"
#include <SDL2/SDL.h>

HalGPIO::HalGPIO() {}
void HalGPIO::begin() {}

bool HalGPIO::isPressed(int buttonPin) const {
    const uint8_t* state = SDL_GetKeyboardState(NULL);
    if (buttonPin == BTN_UP) return state[SDL_SCANCODE_UP];
    if (buttonPin == BTN_DOWN) return state[SDL_SCANCODE_DOWN];
    if (buttonPin == BTN_ENTER) return state[SDL_SCANCODE_RETURN];
    if (buttonPin == BTN_POWER) return state[SDL_SCANCODE_P];
    return false;
}

void HalGPIO::update() {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) exit(0);
    }
}

uint16_t HalGPIO::getHeldTime() const { return 0; }
bool HalGPIO::wasAnyPressed() const { return false; }
bool HalGPIO::wasAnyReleased() const { return false; }
int HalGPIO::getWakeupReason() const { return HalGPIO::WakeupReason::Other; }
int HalGPIO::getBatteryVoltage() const { return 4200; }
bool HalGPIO::isUsbConnected() const { return true; }
""",

    # ------------------- HalSystem.cpp -------------------
    "HalSystem.cpp": """
#include "HalSystem.h"

void HalSystem::begin() {}
void HalSystem::restart() { exit(0); }
void HalSystem::checkPanic() {}
void HalSystem::clearPanic() {}
""",

    # ------------------- HalPowerManager.cpp -------------------
    "HalPowerManager.cpp": """
#include "HalPowerManager.h"
#include "HalGPIO.h"

HalPowerManager powerManager;
bool HalPowerManager::Lock::lowPowerEnabled = false;

void HalPowerManager::begin() {}
void HalPowerManager::startDeepSleep(const HalGPIO& gpio) {}
void HalPowerManager::setPowerSaving(bool enable) {}
""",

    # ------------------- HalStorage.cpp -------------------
    "HalStorage.cpp": """
#include "HalStorage.h"
#include <fstream>
#include <sys/stat.h>
#include <dirent.h>

HalStorage HalStorage::instance;
HalStorage::HalStorage() {}
bool HalStorage::begin() { return true; }
bool HalStorage::ready() const { return true; }

class HalFile::Impl {
public:
    std::fstream stream;
    std::string path;
    bool open(const char* p, int flags) {
        path = p;
        std::ios_base::openmode mode = std::ios_base::binary;
        if (flags & O_WRONLY) mode |= std::ios_base::out;
        else if (flags & O_RDWR) mode |= std::ios_base::out | std::ios_base::in;
        else mode |= std::ios_base::in;
        if (flags & O_CREAT) {
            // Ensure file exists
            std::fstream p(path, std::ios_base::out | std::ios_base::app | std::ios_base::binary);
            p.close();
        }
        if (flags & O_TRUNC) mode |= std::ios_base::trunc;
        if (flags & O_APPEND) mode |= std::ios_base::app;
        
        stream.open(path, mode);
        return stream.is_open();
    }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() {}
HalFile::HalFile(HalFile&& other) : impl(std::move(other.impl)) {}
HalFile& HalFile::operator=(HalFile&& other) {
    if (this != &other) impl = std::move(other.impl);
    return *this;
}

void HalFile::flush() { if(impl && impl->stream.is_open()) impl->stream.flush(); }
size_t HalFile::getName(char* name, size_t len) { return 0; }
size_t HalFile::size() {
    if (!impl || !impl->stream.is_open()) return 0;
    auto pos = impl->stream.tellg();
    impl->stream.seekg(0, std::ios::end);
    size_t s = impl->stream.tellg();
    impl->stream.seekg(pos);
    return s;
}
size_t HalFile::fileSize() { return size(); }
bool HalFile::seek(size_t pos) { if(!impl) return false; impl->stream.seekg(pos); return true; }
bool HalFile::seekCur(int64_t offset) { if(!impl) return false; impl->stream.seekg(offset, std::ios::cur); return true; }
bool HalFile::seekSet(size_t offset) { if(!impl) return false; impl->stream.seekg(offset, std::ios::beg); return true; }
int HalFile::available() const { if(!impl) return 0; auto pos = impl->stream.tellg(); impl->stream.seekg(0, std::ios::end); int s = (int)impl->stream.tellg() - (int)pos; impl->stream.seekg(pos); return s; }
size_t HalFile::position() const { if(!impl) return 0; return impl->stream.tellg(); }
int HalFile::read(void* buf, size_t count) { if(!impl) return -1; impl->stream.read((char*)buf, count); return impl->stream.gcount(); }
int HalFile::read() { if(!impl) return -1; char c; if(impl->stream.get(c)) return (uint8_t)c; return -1; }
size_t HalFile::write(const void* buf, size_t count) { if(!impl) return 0; impl->stream.write((const char*)buf, count); return count; }
size_t HalFile::write(uint8_t b) { if(!impl) return 0; impl->stream.put(b); return 1; }
bool HalFile::rename(const char* newPath) { return false; }
bool HalFile::isDirectory() const { return false; }
void HalFile::rewindDirectory() {}
bool HalFile::close() { if(impl) impl->stream.close(); return true; }
HalFile HalFile::openNextFile() { return HalFile(); }
bool HalFile::isOpen() const { return impl && impl->stream.is_open(); }
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
    HalFile f;
    f.impl->open(std::string("./fs_" + std::string(path)).c_str(), oflag);
    return f;
}
bool HalStorage::mkdir(const char* path, const bool pFlag) { 
    std::string full = "./fs_" + std::string(path);
    // basic mock, not recursive
    ::mkdir(full.c_str(), 0777);
    return true; 
}
bool HalStorage::exists(const char* path) {
    std::string full = "./fs_" + std::string(path);
    struct stat buffer;   
    return (stat (full.c_str(), &buffer) == 0); 
}
bool HalStorage::remove(const char* path) { 
    std::string full = "./fs_" + std::string(path);
    return ::remove(full.c_str()) == 0; 
}
bool HalStorage::rename(const char* oldPath, const char* newPath) { 
    std::string o = "./fs_" + std::string(oldPath);
    std::string n = "./fs_" + std::string(newPath);
    return ::rename(o.c_str(), n.c_str()) == 0; 
}
bool HalStorage::rmdir(const char* path) { return remove(path); }
bool HalStorage::removeDir(const char* path) { return remove(path); }

String HalStorage::readFile(const char* path) {
    HalFile f = open(path, O_RDONLY);
    if (!f) return String("");
    size_t s = f.size();
    std::string content(s, '\\0');
    f.read((void*)content.data(), s);
    return String(content);
}
bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) { return false; }
size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) { return 0; }
bool HalStorage::writeFile(const char* path, const String& content) {
    HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    f.write(content.c_str(), content.length());
    return true;
}
bool HalStorage::ensureDirectoryExists(const char* path) { return mkdir(path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
    file = open(path, O_RDONLY);
    return file.isOpen();
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
    file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    return file.isOpen();
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
    return {};
}

"""
}

for name, content in files.items():
    with open(os.path.join(MOCK_DIR, name), "w") as f:
        f.write(content.strip() + "\n")

with open(os.path.join(MOCK_DIR, "simulator_main.cpp"), "w") as f:
    f.write("""
#include "Arduino.h"
extern void setup();
extern void loop();

int main(int argc, char** argv) {
    setup();
    while(true) {
        loop();
    }
    return 0;
}
""")

print("HAL cpp files generated.")
