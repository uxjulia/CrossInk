"""Execute the production reader idle function with deterministic boundary doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text()
start = source.index("void EpubReaderActivity::idlePrewarmTtf(")
body = source.index("{", start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
# Every repaint and failed render must invalidate the previous successful page.
render = source[source.index("void EpubReaderActivity::render(RenderLock&& lock)"):]
assert render.index("idleTtfReady.store(false") < render.index("idleTtfReady.store(true")
header = (root / "src/activities/reader/EpubReaderActivity.h").read_text()
request = header[header.index("void requestUpdate(bool immediate"):]
assert request.index("idleTtfReady.store(false") < request.index("Activity::requestUpdate(immediate)")
code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#define LOG_DBG(...) ((void)0)
unsigned long millis() { return 100; }
bool edge = false;
bool idleFontInputWakePending() { return edge; }
struct MemoryBudget {
  static int snapshot() { return 1; }
  static bool hasHeap(int, int, int) { return heap; }
  static inline bool heap = true;
};
constexpr int IDLE_SD_FONT_PREWARM_MIN_FREE = 1, IDLE_SD_FONT_PREWARM_MIN_MAX_ALLOC = 1;
struct HalScalableFont { struct IdlePrewarmScope {}; };
struct FontCacheManager {
  enum class PreparationPolicy { IdleScalable };
  enum class IdleResult { Complete, Yielded, Failed };
  struct IdleScalableJob { int count = 0; void reset() { count = 0; } };
  struct Scope { void endScanForIdle(IdleScalableJob& j) { j.count = 1; } };
  Scope createPrewarmScope(PreparationPolicy) { return {}; }
  int slices = 0;
  IdleResult result = IdleResult::Complete;
  IdleResult prewarmIdleSlice(IdleScalableJob&, bool (*yield)(void*), void* ctx, int, bool (*)(void*)) {
    ++slices; return yield(ctx) ? IdleResult::Yielded : result;
  }
};
struct Renderer { bool present = true; bool hasFrameBuffer() { return present; } };
struct Page {
  bool renderText(Renderer&, int, int, int, bool, bool (*stop)(void*), void* ctx) { return !stop(ctx); }
};
struct Section {
  int currentPage = 0, pageCount = 10, loads = 0;
  bool building = false, failed = false;
  bool isBuilding() { return building; }
  std::unique_ptr<Page> loadPage(int) { ++loads; return failed ? nullptr : std::make_unique<Page>(); }
};
struct EpubReaderActivity {
  std::unique_ptr<Section> section = std::make_unique<Section>();
  std::atomic<bool> idleTtfReady{true}, pageBufferStale{false};
  const Section* idleTtfSection = section.get();
  int currentSpineIndex = 0, idleTtfSpine = 0, idleTtfVisiblePage = 0, idleTtfFont = 1;
  uint8_t idleTtfOffset = 1;
  bool activeFootnotePreview = false, automaticPageTurnActive = false;
  Renderer renderer;
  struct Input { bool pending = false; bool hasPendingActivity() const { return pending; } } mappedInput;
  FontCacheManager::IdleScalableJob idleTtfJob;
  void idlePrewarmTtf(int, FontCacheManager&);
};
'''
code += function
code += r'''
int main() {
  const auto blocked = [](auto mutate) {
    EpubReaderActivity r; FontCacheManager f; mutate(r);
    r.idlePrewarmTtf(1, f); assert(f.slices == 0);
    if (r.section) assert(r.section->loads == 0);
  };
  blocked([](auto& r) { r.section.reset(); });
  blocked([](auto& r) { r.idleTtfSection = nullptr; });
  blocked([](auto& r) { r.currentSpineIndex = 1; });
  blocked([](auto& r) { r.section->currentPage = 1; });
  blocked([](auto& r) { r.idleTtfFont = 2; });
  blocked([](auto& r) { r.section->building = true; });
  blocked([](auto& r) { r.idleTtfReady = false; });
  blocked([](auto& r) { r.pageBufferStale = true; });
  blocked([](auto& r) { r.renderer.present = false; });
  blocked([](auto& r) { r.activeFootnotePreview = true; });
  blocked([](auto& r) { r.automaticPageTurnActive = true; });
  blocked([](auto& r) { r.mappedInput.pending = true; });
  blocked([](auto& r) { r.idleTtfOffset = 6; });
  blocked([](auto& r) { r.section->pageCount = 1; });
  edge = true; blocked([](auto&) {}); edge = false;
  MemoryBudget::heap = false; blocked([](auto&) {}); MemoryBudget::heap = true;
  EpubReaderActivity r; FontCacheManager f;
  for (int i = 0; i < 10; ++i) r.idlePrewarmTtf(1, f);
  assert(f.slices == 5 && r.section->loads == 5 && r.idleTtfOffset == 6);
  EpubReaderActivity failed; failed.section->failed = true;
  failed.idlePrewarmTtf(1, f); assert(!failed.idleTtfReady && failed.section->loads == 1);
  failed.idlePrewarmTtf(1, f); assert(failed.section->loads == 1);
  EpubReaderActivity raster; f.result = FontCacheManager::IdleResult::Failed;
  raster.idlePrewarmTtf(1, f); assert(!raster.idleTtfReady && raster.idleTtfJob.count == 0);
  EpubReaderActivity resumed; f.result = FontCacheManager::IdleResult::Yielded;
  resumed.idlePrewarmTtf(1, f); resumed.idlePrewarmTtf(1, f);
  assert(resumed.section->loads == 1 && resumed.idleTtfOffset == 1);
}
'''
with tempfile.TemporaryDirectory(prefix="crossink-idle-guards-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    cpp.write_text(code)
    binary = Path(tmp) / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Reader idle TTF lifecycle, input, heap, failure, and lookahead guards passed")
