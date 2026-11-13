#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

// drawFrame(grayscale, completeFrame) replays the image into the current plane.
// Complete frames include chrome; overlay masks must contain only image shades.
// Re-decoding avoids a second full framebuffer and keeps PNG memory bounded.
template <typename DrawFrame>
bool renderImageViewerGrayscale(GfxRenderer& renderer, DrawFrame&& drawFrame) {
  const auto monochrome = [&]() {
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    if (!drawFrame(false, true)) return false;
    renderer.cleanupGrayscaleWithFrameBuffer();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  };
  const bool absolute = !renderer.isDisplayInverted() && renderer.supportsAbsoluteGrayscale();
  const bool grayscale = absolute || (!renderer.isDisplayInverted() && renderer.supportsTextGrayscale());
  if (!grayscale) return monochrome();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!drawFrame(true, true)) return false;
  if (absolute) {
    const bool started = (renderer.supportsDirectGrayscale() && renderer.displayDirectGrayscaleBase()) ||
                         renderer.displayAbsoluteGrayscaleBase();
    if (!started) return monochrome();
  } else {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  }
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.setRenderMode(mode);
    renderer.clearScreen(absolute ? 0xff : 0x00);
    if (!drawFrame(true, absolute)) {
      LOG_ERR("PNG", "Grayscale replay failed; trying monochrome");
      return monochrome();
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB)
      renderer.copyGrayscaleLsbBuffers();
    else
      renderer.copyGrayscaleMsbBuffers();
  }
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  // Popups and differential updates must see the original BW page, never a selector plane.
  renderer.clearScreen();
  if (!drawFrame(true, true)) return monochrome();
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}
