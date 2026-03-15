#include "HalDisplay.h"
#include <GfxRenderer.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <cstring>
#include <iostream>

static SDL_Window*   window       = nullptr;
static SDL_Renderer* sdl_renderer = nullptr;
static SDL_Texture*  texture      = nullptr;

// Pixel buffer written by the render task, read by the main thread for SDL_RenderPresent.
// On macOS, SDL calls must happen on the main thread.
static uint32_t pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by shouldQuit().
std::atomic<bool> quitRequested{false};

static GfxRenderer::Orientation currentOrientation = GfxRenderer::Portrait;

void HalDisplay::setSimulatorOrientation(int o) {
    currentOrientation = static_cast<GfxRenderer::Orientation>(o);
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }

    bool isPortrait = (currentOrientation == GfxRenderer::Portrait ||
                       currentOrientation == GfxRenderer::PortraitInverted);
    int winW = isPortrait ? DISPLAY_HEIGHT / 2 : DISPLAY_WIDTH  / 2;
    int winH = isPortrait ? DISPLAY_WIDTH  / 2 : DISPLAY_HEIGHT / 2;

    // SDL_WINDOW_ALLOW_HIGHDPI lets the renderer use full Retina/HiDPI pixels on macOS
    // so we get crisp 1:1 rendering instead of a blurry upscale.
    window = SDL_CreateWindow("Simulator - Open-X4 SDK",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              winW, winH, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    // Keep all rendering logic in logical (winW×winH) coordinates; SDL maps to drawable pixels.
    SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);

    // Linear filtering: Bayer-dithered pixels average to correct gray at scaled sizes
    // rather than showing harsh black/white patterns.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void HalDisplay::clearScreen(uint8_t color) const {
    memset(getFrameBuffer(), color, BUFFER_SIZE);
}

void HalDisplay::drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool) const {}
void HalDisplay::drawImageTransparent(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool) const {}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
    refreshDisplay(mode, turnOffScreen);
}

// Called from the render task (background thread): convert framebuffer to pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
    const uint8_t* fb = getFrameBuffer();
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            int byteIdx = (y * DISPLAY_WIDTH + x) / 8;
            int bitIdx  = 7 - (x % 8);
            bool isWhite = (fb[byteIdx] & (1 << bitIdx)) != 0;
            pixelBuf[y * DISPLAY_WIDTH + x] = isWhite ? 0xFFFFFFFF : 0xFF000000;
        }
    }
    pendingPresent.store(true);
}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
    if (!pendingPresent.load()) return;
    pendingPresent.store(false);

    if (!texture || !sdl_renderer) return;

    SDL_UpdateTexture(texture, nullptr, pixelBuf, DISPLAY_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(sdl_renderer);

    // For portrait modes the texture (800×480 landscape) must be rotated to fill the
    // portrait window (240×400).  SDL_RenderCopyEx rotates around the *centre* of dst,
    // so dst must be landscape-oriented (400×240) and offset so its centre coincides
    // with the window centre (120, 200).  After rotation the result is 240×400.
    //
    // Portrait rotateCoordinates stores content rotated 90° CCW in the physical buffer,
    // so we rotate +90° CW here to undo it.
    // PortraitInverted stores content rotated 90° CW → undo with -90°.
    switch (currentOrientation) {
        case GfxRenderer::Portrait: {
            // dst centre = window centre (120,200), landscape size 400×240
            SDL_Rect dst = {(DISPLAY_HEIGHT - DISPLAY_WIDTH) / 4,  // -80
                            (DISPLAY_WIDTH - DISPLAY_HEIGHT) / 4,  //  80
                            DISPLAY_WIDTH / 2,                     // 400
                            DISPLAY_HEIGHT / 2};                   // 240
            SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 90.0, nullptr, SDL_FLIP_NONE);
            break;
        }
        case GfxRenderer::PortraitInverted: {
            SDL_Rect dst = {(DISPLAY_HEIGHT - DISPLAY_WIDTH) / 4,
                            (DISPLAY_WIDTH - DISPLAY_HEIGHT) / 4,
                            DISPLAY_WIDTH / 2,
                            DISPLAY_HEIGHT / 2};
            SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, -90.0, nullptr, SDL_FLIP_NONE);
            break;
        }
        default: {
            SDL_Rect dst = {0, 0, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2};
            SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
            break;
        }
    }

    SDL_RenderPresent(sdl_renderer);
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const {
    static uint8_t buffer[HalDisplay::BUFFER_SIZE];
    return buffer;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t*) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t*) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}
void HalDisplay::displayGrayBuffer(bool) {}
