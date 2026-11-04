#!/usr/bin/env python3
"""Exercise production loan lifecycle and reader redraw/popup gates (ASan/UBSan)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


FIXTURES = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <variant>
#include <vector>
#include <string>
#include <algorithm>
#include <new>
#include "BuildScratch.h"
#define LOG_ERR(...) assert(false)
struct Esp { void restart() { assert(false); } } ESP;
struct Display {
  uint8_t bytes[48000]{};
  bool lent = false;
  uint8_t* getFrameBuffer() { return lent ? nullptr : bytes; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) { lent = true; *size = sizeof(bytes); return bytes; }
  void returnFrameBufferStorage() { lent = false; memset(bytes, 0xff, sizeof(bytes)); }
};
class GfxRenderer {
 public:
  Display display;
  uint8_t* frameBuffer = display.bytes;
  uint32_t frameBufferLoans = 0;
  uint32_t frameBufferLoanCount() const { return frameBufferLoans; }
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }
  void clearScreen(int) { frameBuffer[0] = 0xff; }
  void displayBuffer() {}

  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&);
    ~FrameBufferLoan() { end(); }
    void end();
   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };
};
struct Popup {
  bool active = false;
  unsigned paints = 0;
  bool isActive() const { return active; }
  bool processRender(GfxRenderer& renderer, int) {
    if (!active) return false;
    assert(renderer.frameBuffer && renderer.frameBuffer[0] == 0x42);
    ++paints;
    return true;
  }
};
namespace ReaderUtils { int readerBackgroundColor() { return 0xff; } }
constexpr int STR_PAGE_LOAD_ERROR = 1, STR_MEMORY_ERROR = 2;
int tr(int x) { return x; }
struct Ui {
  void drawPopup(GfxRenderer& renderer, int) { renderer.frameBuffer[0] = 0x99; }
} GUI;
enum class RequestUpdateResult { Rendered, Rejected };
struct RenderLock { template<typename T> explicit RenderLock(T&) {} };
struct Manager {
  bool active = true;
  template<typename T> bool isCurrentActivity(T*) const { return active; }
} activityManager;
struct Block { bool isEmpty() const { return true; } const char* wordText(int) const { return ""; } };
struct Element { int getTag() const { return 0; } };
struct PageLine : Element { Block* getBlock() const { return nullptr; } };
constexpr int TAG_PageLine = 1;
struct Page { std::vector<std::unique_ptr<Element>> elements; };
struct Section {
  int currentPage = 0, pageCount = 1;
  std::unique_ptr<Page> loadPageFromSectionFile() { return std::make_unique<Page>(); }
};
struct Epub { std::string getCachePath() const { return "/cache"; } };
struct ReaderViewportLayout { int marginLeft = 0, marginTop = 0, marginBottom = 0; };
ReaderViewportLayout computeReaderViewportLayout(GfxRenderer&, bool) { return {}; }
struct BookReaderSettingsData { std::string dictionarySdFontFamilyName; int dictionaryFontPointSize = 0; };
BookReaderSettingsData loadBookReaderSettingsFile(const std::string&) { return {}; }
struct DictionaryClippingRequest {};
struct ActivityResult { std::variant<std::monostate, DictionaryClippingRequest> data; };
namespace MemoryBudget { void logHeapShape(const char*) {} }
void releaseReaderSdFontCachesForLowMemory(GfxRenderer&, const char*, const char*) {}
void drawToast(GfxRenderer&, int) {}
void delay(int) {}
struct DictionaryWordSelectActivity {
  bool reuse;
  template<typename... Args>
  DictionaryWordSelectActivity(GfxRenderer&, int, std::unique_ptr<Page>, int, int, std::string,
                               std::string, bool, bool framebufferContainsPage, Args&&...)
      : reuse(framebufferContainsPage) {}
};
template<typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
struct Activity { virtual bool canSnapshotForSleepOverlay() const { return false; } };
struct EpubReaderActivity : Activity {
  GfxRenderer renderer;
  int mappedInput = 0;
  Popup quickActionsPopup;
  std::atomic<bool> pageBufferStale{false};
  bool pendingRenderModeToast = false, pendingSafeModeToast = false, pendingFontDamagedToast = false;
  unsigned updates = 0, syncRenders = 0, handoffs = 0;
  bool syncRenderCompletes = true, syncRenderRejected = false;
  bool restoreSucceeds = false, dictionaryReusesPage = true, automaticPageTurnActive = false;
  Section sectionStorage;
  Epub epubStorage;
  Section* section = &sectionStorage;
  Epub* epub = &epubStorage;
  std::atomic<uint8_t> pendingHeapShapeReaderRedrawStages{0};
  static constexpr uint8_t HEAP_SHAPE_REDRAW_DICT = 2;
  bool restoreCurrentPageBufferAfterSilentIndex() {
    if (!restoreSucceeds) return false;
    renderer.frameBuffer[0] = 0x42;
    return true;
  }
  RequestUpdateResult requestUpdateAndWait() {
    ++syncRenders;
    if (!syncRenderCompletes || syncRenderRejected) {
      renderer.frameBuffer[0] = 0xff;
      return syncRenderRejected ? RequestUpdateResult::Rejected : RequestUpdateResult::Rendered;
    }
    renderer.frameBuffer[0] = 0x42;
    pageBufferStale.store(false, std::memory_order_relaxed);
    return RequestUpdateResult::Rendered;
  }
  void pauseReadingPaceTimer(const char*) {}
  void resumeReadingPaceTimer(const char*) {}
  void clearPendingManualPageTurns() {}
  void startClipSelection(const DictionaryClippingRequest*) {}
  static std::unique_ptr<Page> reloadDictionaryLookupPageCallback(void*, int) { return {}; }
  template<typename Callback>
  void startActivityForResult(std::unique_ptr<DictionaryWordSelectActivity> child, Callback) {
    ++handoffs; dictionaryReusesPage = child->reuse;
  }
  SNAPSHOT_METHOD
  void openWordSelect(bool, int, int, bool);
  void onInputLockChanged(bool);
  void restoreStalePageBufferForInputLock();
  void requestUpdate() { ++updates; }
  void invalidatePageBufferAfterBuild(uint32_t);
  bool backgroundBuildCanUsePageBuffer() const;
  bool renderQuickActionsPopup();
};
'''
TESTS = r'''
int main() {
  EpubReaderActivity reader;
  reader.renderer.frameBuffer[0] = 0x42;
  const uint32_t before = reader.renderer.frameBufferLoanCount();
  assert(reader.backgroundBuildCanUsePageBuffer());
  reader.invalidatePageBufferAfterBuild(before);
  assert(reader.updates == 0 && !reader.pageBufferStale);
  {
    GfxRenderer::FrameBufferLoan outer(reader.renderer);
    assert(!reader.renderer.hasFrameBuffer());
    uint8_t* scratch = buildscratch::claim(43000);
    assert(scratch == reader.renderer.display.bytes);
    memset(scratch, 0x12, 43000);
    {
      GfxRenderer::FrameBufferLoan nested(reader.renderer);
      nested.end();
      assert(!reader.renderer.hasFrameBuffer());
      assert(reader.renderer.frameBufferLoanCount() == before + 1);
    }
    buildscratch::release(scratch);
    outer.end();
    assert(reader.renderer.hasFrameBuffer());
    assert(!buildscratch::available(1));
    assert(reader.renderer.frameBuffer[0] == 0xff);
  }
  reader.invalidatePageBufferAfterBuild(before);
  assert(reader.updates == 1 && reader.pageBufferStale);
  assert(reader.canSnapshotForSleepOverlay());  // Preserve global screenshot dispatch to the reader.
  assert(reader.renderer.frameBuffer[0] == 0x99);  // Error frame, never a white base.
  reader.openWordSelect(true, 0, 0, false);
  assert(reader.handoffs == 1 && !reader.dictionaryReusesPage);
  activityManager.active = false;
  reader.onInputLockChanged(true);
  assert(reader.syncRenders == 0 && reader.pageBufferStale);
  activityManager.active = true;
  reader.onInputLockChanged(true);
  assert(reader.syncRenders == 1 && !reader.pageBufferStale);
  reader.openWordSelect(true, 0, 0, false);
  assert(reader.handoffs == 2 && reader.dictionaryReusesPage);
  for (bool rejected : {false, true}) {
    reader.pageBufferStale = true;
    reader.syncRenderCompletes = false;
    reader.syncRenderRejected = rejected;
    reader.onInputLockChanged(true);
    assert(reader.pageBufferStale && reader.renderer.frameBuffer[0] == 0x99);
  }
  reader.syncRenderCompletes = true;
  reader.syncRenderRejected = false;
  reader.pageBufferStale = true;
  assert(!reader.backgroundBuildCanUsePageBuffer());
  reader.quickActionsPopup.active = true;  // Same-tick popup must not paint the blank returned buffer.
  assert(!reader.renderQuickActionsPopup() && reader.quickActionsPopup.paints == 0);
  reader.renderer.frameBuffer[0] = 0x42;  // Complete page composition, under RenderLock.
  reader.pageBufferStale.store(false, std::memory_order_relaxed);
  assert(reader.renderQuickActionsPopup() && reader.quickActionsPopup.paints == 1);
  assert(!reader.backgroundBuildCanUsePageBuffer());
  reader.quickActionsPopup.active = false;
  for (bool* toast : {&reader.pendingRenderModeToast, &reader.pendingSafeModeToast, &reader.pendingFontDamagedToast}) {
    *toast = true;
    assert(!reader.backgroundBuildCanUsePageBuffer());
    *toast = false;
    assert(reader.backgroundBuildCanUsePageBuffer());
  }
  const auto earlyReturn = [&]() { GfxRenderer::FrameBufferLoan loan(reader.renderer); return false; };
  assert(!earlyReturn());
  assert(reader.renderer.hasFrameBuffer() && reader.renderer.frameBufferLoanCount() == before + 2);
  reader.restoreSucceeds = true;
  reader.invalidatePageBufferAfterBuild(before + 1);
  assert(!reader.pageBufferStale && reader.renderer.frameBuffer[0] == 0x42);
  assert(reader.updates == 1);  // Successful RAM restoration needs no panel redraw.
}
'''

FIXTURES = FIXTURES.replace("SNAPSHOT_METHOD", method("src/activities/reader/EpubReaderActivity.h",
                                                     "bool canSnapshotForSleepOverlay() const override"))
bodies = "".join(method("lib/GfxRenderer/GfxRenderer.cpp", signature) for signature in [
    "void GfxRenderer::releaseFrameBufferForBuild()", "bool GfxRenderer::restoreFrameBufferAfterBuild()",
    "GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer)", "void GfxRenderer::FrameBufferLoan::end()"])
bodies += "".join(method("src/activities/reader/EpubReaderActivity.cpp", signature) for signature in [
    "void EpubReaderActivity::invalidatePageBufferAfterBuild", "bool EpubReaderActivity::backgroundBuildCanUsePageBuffer",
    "bool EpubReaderActivity::renderQuickActionsPopup",
    "void EpubReaderActivity::restoreStalePageBufferForInputLock",
    "void EpubReaderActivity::onInputLockChanged", "void EpubReaderActivity::openWordSelect"])
with tempfile.TemporaryDirectory(prefix="crossink-image-loan-") as directory:
    source = Path(directory) / "test.cpp"
    source.write_text(FIXTURES + bodies + TESTS)
    binary = Path(directory) / "test"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(ROOT / "lib/Memory"),
                    "-I" + str(ROOT / "test/chapter_html_slim_parser/stubs"), str(source),
                    str(ROOT / "lib/Memory/BuildScratch.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Framebuffer loan: nesting/early-return/scratch/retry redraw/popup/toast/dictionary/active-vs-stacked Quick Lock gates passed (ASan/UBSan)")
