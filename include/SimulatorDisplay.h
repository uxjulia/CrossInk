#pragma once
#include <SDL2/SDL.h>
#include <iostream>

class SimulatorDisplay {
public:
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    void begin() {
        SDL_Init(SDL_INIT_VIDEO);
        window = SDL_CreateWindow("CrossInk Simulator", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, 480, 800, 0);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        // Set background to e-ink white
        SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }

    // Mocking the E-Ink drawing function
    void drawPixel(int x, int y, uint16_t color) {
        if (color == 0) SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // Black
        else SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);     // White

        SDL_RenderDrawPoint(renderer, x, y);
    }

    void display() {
        SDL_RenderPresent(renderer);
    }

    void handleEvents() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) exit(0);
        }
    }
};