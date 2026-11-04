#pragma once
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

namespace ReaderUtils {
// Lazy PSRAM storage, reused for page turns and released with its reader.
struct DeferredGrayscalePlanes {
  HeapByteBuffer lsb;
  HeapByteBuffer msb;
  size_t bytes = 0;
};

// Two whole AA planes in PSRAM, so the gray planes can render while a base
// refresh still runs. False (nothing allocated) without room for both.
inline bool allocateDeferredGrayscalePlanes(const GfxRenderer& renderer, DeferredGrayscalePlanes& planes) {
  if (!psramHeapAvailable()) return false;
  constexpr size_t PSRAM_PLANE_RESERVE = 128 * 1024;
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
  if (planes.bytes == planeBytes && planes.lsb && planes.msb) return true;
  planes = {};
  auto& lsb = planes.lsb;
  auto& msb = planes.msb;
  const auto psram = byteHeapSnapshot(MemoryPool::Psram);
  if (psram.free < 2 * planeBytes + PSRAM_PLANE_RESERVE || psram.largest < planeBytes) return false;
  lsb = makePsramByteBufferNoThrow(planeBytes);
  if (lsb) msb = makePsramByteBufferNoThrow(planeBytes);
  if (!lsb || !msb) {
    LOG_ERR("READER", "Deferred grayscale allocation failed; using blocking pass");
    lsb.reset();
    msb.reset();
    return false;
  }
  planes.bytes = planeBytes;
  return true;
}

// X4 Pro: the BW page in the framebuffer goes out as the gray base in the
// background while both AA planes render into PSRAM, as EPUB text pages do.
// The framebuffer stays untouched, so no BW snapshot/restore. renderPlane()
// draws one plane (clearScreen(0x00) already done, render mode set) and
// returns false on failure, which drops the gray pass. Returns false, with
// nothing displayed, on cleanup turns or when the panel/PSRAM can't defer:
// the caller takes its blocking path.
template <typename PlaneFn>
bool displayDeferredGrayscale(GfxRenderer& renderer, int& pagesUntilFullRefresh, DeferredGrayscalePlanes& planes,
                              PlaneFn&& renderPlane) {
  if (pagesUntilFullRefresh <= 1 || !renderer.supportsDeferredGrayscaleBase()) return false;
  if (!allocateDeferredGrayscalePlanes(renderer, planes)) return false;
  auto& lsb = planes.lsb;
  auto& msb = planes.msb;
  LOG_DBG("READER", "Deferred grayscale: preparing planes during base refresh");
  renderer.displayGrayscaleBaseAsync(HalDisplay::FAST_REFRESH);
  pagesUntilFullRefresh--;
  bool ok = true;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (!ok) break;
    renderer.setRenderMode(mode);
    renderer.beginStripTarget(mode == GfxRenderer::GRAYSCALE_LSB ? lsb.get() : msb.get(), 0,
                              renderer.getDisplayHeight());
    renderer.clearScreen(0x00);
    ok = renderPlane();
    renderer.endStripTarget();
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.waitRefreshComplete();
  if (ok) {
    renderer.copyGrayscalePlanes(lsb.get(), msb.get());
    renderer.displayGrayBuffer();
  }
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

}  // namespace ReaderUtils
