#include "EpubGrayscale.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>

#include <algorithm>
#include <cstring>

namespace EpubGrayscale {

HeapByteBuffer allocateScratch(const int widthBytes, const int height, size_t& size, bool& inPsram) {
  size = 0;
  inPsram = false;
  if (widthBytes <= 0 || height <= 0) return {};

  const size_t stripBytes = static_cast<size_t>(widthBytes) * std::min(height, GRAYSCALE_STRIP_ROWS);
  const size_t planeBytes = static_cast<size_t>(widthBytes) * height;
  if (psramHeapAvailable()) {
    // One runtime-sized mask (52,992 bytes on Eego), reused for both gray
    // planes. Too large for the task stack; never request this from internal RAM.
    constexpr size_t PSRAM_RESERVE = 128 * 1024;
    const auto psram = byteHeapSnapshot(MemoryPool::Psram);
    if (psram.free >= planeBytes + PSRAM_RESERVE && psram.largest >= planeBytes) {
      auto buffer = makePsramByteBufferNoThrow(planeBytes);
      if (buffer) {
        size = planeBytes;
        inPsram = true;
        return buffer;
      }
      LOG_ERR("EPS", "OOM: PSRAM grayscale plane (%u bytes); trying strip", static_cast<unsigned>(planeBytes));
    }
    auto buffer = makePsramByteBufferNoThrow(stripBytes);
    if (buffer) {
      size = stripBytes;
      inPsram = true;
      return buffer;
    }
    LOG_ERR("EPS", "OOM: PSRAM grayscale strip (%u bytes); trying default heap", static_cast<unsigned>(stripBytes));
  }
  auto buffer = makeHeapByteBufferNoThrow(stripBytes);
  if (buffer) {
    size = stripBytes;
  } else {
    LOG_ERR("EPS", "OOM: grayscale strip (%u bytes); falling back to BW snapshot", static_cast<unsigned>(stripBytes));
  }
  return buffer;
}

bool runAbsoluteImageGrayscalePass(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                                   const int marginTop, uint8_t* lsb, uint8_t* msb, const size_t planeSize,
                                   void (*renderOverlays)(void*), void* context) {
  const int height = renderer.getDisplayHeight();
  const size_t bytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * height;
  if (!renderer.grayPlanesAreAbsolute() || !lsb || !msb || planeSize < bytes) {
    LOG_ERR("EPS", "Cannot render complete absolute image planes");
    return false;
  }
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    auto* plane = mode == GfxRenderer::GRAYSCALE_LSB ? lsb : msb;
    // Black/white have the same bit in both absolute planes. Seed the complete
    // page, including non-AA text and transient overlays, before replacing images.
    memcpy(plane, renderer.getFrameBuffer(), bytes);
    renderer.setRenderMode(mode);
    renderer.beginStripTarget(plane, 0, height);
    page.renderImages(renderer, fontId, marginLeft, marginTop);
    // Feedback or clipping highlights may overlap the image itself.
    if (renderOverlays) renderOverlays(context);
    renderer.endStripTarget();
  }
  renderer.copyGrayscalePlanes(lsb, msb);
  // BW mode cancels an unfinished absolute pass, so commit the planes first.
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                           const int marginTop, const bool foregroundBlack, const bool needsTextGrayscale,
                           const bool needsImageGrayscale, uint8_t* scratch, const size_t scratchSize,
                           const bool asyncRefreshPending) {
  if ((!needsTextGrayscale && !needsImageGrayscale) || !renderer.supportsStripGrayscale()) {
    return false;
  }

  const int displayHeight = renderer.getDisplayHeight();
  const int displayWidthBytes = renderer.getDisplayWidthBytes();
  const size_t planeBytes = static_cast<size_t>(displayWidthBytes) * displayHeight;

  const auto renderPlaneToBuffer = [&](const GfxRenderer::RenderMode mode, uint8_t* buffer) {
    renderer.setRenderMode(mode);
    // A complete scratch plane needs only one page traversal. The target is
    // separate from the live BW framebuffer, just like an 80-row strip.
    renderer.beginStripTarget(buffer, 0, displayHeight);
    renderer.clearScreen(0x00);
    if (needsTextGrayscale) {
      page.renderText(renderer, fontId, marginLeft, marginTop, foregroundBlack);
    }
    if (needsImageGrayscale) {
      page.renderImages(renderer, fontId, marginLeft, marginTop);
    }
    renderer.endStripTarget();
  };

  // Async overlap uses up to two additional whole-plane buffers (~48 KB each).
  // Keep these transient and off the task stack, leaving enough total and
  // contiguous heap for the next render allocations.
  constexpr size_t PLANE_BUFFER_FREE_HEAP_RESERVE = 60000;
  constexpr size_t PLANE_BUFFER_MAX_ALLOC_RESERVE = 16 * 1024;
  const bool usePsramPlanes = psramHeapAvailable();
  const auto planeBufferFits = [planeBytes, usePsramPlanes] {
    if (usePsramPlanes) {
      constexpr size_t PSRAM_PLANE_RESERVE = 128 * 1024;
      const auto psram = MemoryBudget::psramSnapshot();
      return psram.freeHeap >= planeBytes + PSRAM_PLANE_RESERVE && psram.maxAllocHeap >= planeBytes;
    }
    return ESP.getFreeHeap() >= planeBytes + PLANE_BUFFER_FREE_HEAP_RESERVE &&
           ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUFFER_MAX_ALLOC_RESERVE;
  };
  const auto allocatePlane = [planeBytes, usePsramPlanes] {
    return usePsramPlanes ? makePsramByteBufferNoThrow(planeBytes) : makeHeapByteBufferNoThrow(planeBytes);
  };
  auto lsbPlaneBuf = (asyncRefreshPending && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};
  auto msbPlaneBuf = (lsbPlaneBuf && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};

  if (lsbPlaneBuf) {
    if (usePsramPlanes) {
      LOG_INF("EPS", "Using PSRAM grayscale planes: bytes=%u count=%u", static_cast<unsigned>(planeBytes),
              msbPlaneBuf ? 2U : 1U);
    }
    renderPlaneToBuffer(GfxRenderer::GRAYSCALE_LSB, lsbPlaneBuf.get());
    if (msbPlaneBuf) {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, msbPlaneBuf.get());
    }

    renderer.waitRefreshComplete();
    renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, displayHeight);
    if (msbPlaneBuf) {
      renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, displayHeight);
    } else {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, lsbPlaneBuf.get());
      renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, displayHeight);
    }

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    return true;
  }

  if (asyncRefreshPending) {
    // Controller writes and the BW snapshot fallback both need the refresh to
    // be complete when the whole-plane allocation cannot be satisfied.
    renderer.waitRefreshComplete();
  }

  const size_t requiredScratchSize =
      static_cast<size_t>(displayWidthBytes) * std::min(displayHeight, GRAYSCALE_STRIP_ROWS);
  if (!scratch || scratchSize < requiredScratchSize) {
    if (asyncRefreshPending) {
      // The shadow-free async update does not rebuild the controller's
      // differential baseline. Re-sync it even when grayscale is skipped.
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
    return false;
  }

  // Use all available scratch rows: a retained PSRAM plane visits the page
  // once per mask instead of once per 80-row band. The live BW framebuffer
  // and the display/cleanup sequence remain unchanged.
  const int scratchRows =
      static_cast<int>(std::min(scratchSize / displayWidthBytes, static_cast<size_t>(displayHeight)));
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, const bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < displayHeight; y += scratchRows) {
      const int rows = std::min(scratchRows, displayHeight - y);
      renderer.beginStripTarget(scratch, y, rows);
      renderer.clearScreen(0x00);
      if (needsTextGrayscale) {
        page.renderText(renderer, fontId, marginLeft, marginTop, foregroundBlack);
      }
      if (needsImageGrayscale) {
        page.renderImages(renderer, fontId, marginLeft, marginTop);
      }
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch, y, rows);
    }
  };

  renderPlane(GfxRenderer::GRAYSCALE_LSB, true);

  renderPlane(GfxRenderer::GRAYSCALE_MSB, false);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

}  // namespace EpubGrayscale
