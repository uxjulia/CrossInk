#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

namespace EpubGrayscale {
constexpr int GRAYSCALE_STRIP_ROWS = 80;

// Reader-owned, reusable scratch. Prefer one complete mask in PSRAM, but keep
// the small strip fallback out of that pool when external allocation fails.
HeapByteBuffer allocateScratch(int widthBytes, int height, size_t& size, bool& inPsram);

// Complete absolute image planes inherit text, bars and feedback from the BW
// frame. Caller owns two PSRAM planes and has started an absolute gray pass.
bool runAbsoluteImageGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                                   uint8_t* lsb, uint8_t* msb, size_t planeSize,
                                   void (*renderOverlays)(void*) = nullptr, void* context = nullptr);

// Preserves the live BW buffer and existing controller synchronization. False
// leaves the caller responsible for its existing BW-snapshot fallback.
bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                           bool foregroundBlack, bool needsTextGrayscale, bool needsImageGrayscale, uint8_t* scratch,
                           size_t scratchSize, bool asyncRefreshPending);
}  // namespace EpubGrayscale
