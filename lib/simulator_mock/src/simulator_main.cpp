
#include "Arduino.h"
#include "HalDisplay.h"
#include <SDL2/SDL.h>
#include <unistd.h>

extern void setup();
extern void loop();
extern HalDisplay display;  // defined in main.cpp

int main(int argc, char** argv) {
    setup();
    while (!display.shouldQuit()) {
        loop();
        // SDL must be driven from the main thread on macOS.
        // The render task writes pixels and sets pendingPresent; we flush them here.
        display.presentIfNeeded();
        // Yield to the OS so macOS delivers pending keyboard/window events to SDL.
        // Without this, the tight spin-loop starves the Cocoa event system and key
        // presses are only picked up sporadically. 1 ms also caps the loop at ~1 kHz,
        // which matches realistic device behaviour (the real ESP32-C3 is limited by
        // FreeRTOS tick rate and e-ink refresh time).
        SDL_Delay(1);
    }
    SDL_Quit();
    // Use _exit() instead of return/exit() to bypass C++ global destructors.
    // `activityManager` (and other globals in main.cpp) are constructed before
    // the render task thread starts, and the render task runs a [[noreturn]]
    // infinite loop.  If normal exit() runs global destructors while the render
    // thread is mid-render, the destructor races with the thread → SIGABRT/
    // SIGSEGV → "quit unexpectedly" dialog.  SDL is already torn down above, so
    // calling _exit(0) here is safe.
    _exit(0);
}
