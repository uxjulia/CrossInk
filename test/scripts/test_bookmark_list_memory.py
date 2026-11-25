#!/usr/bin/env python3
"""Exercise production bookmark row building with heap allocation denied.

A minimal screen isolates the list's own temporary buffers from renderer/font
allocations. This is a host regression guard, not an ESP32 memory simulation.
"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source_path = "src/activities/reader/EpubReaderBookmarkListActivity.cpp"

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

prefix = r'''
#include <components/lists/list.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "BookmarkStore.h"
namespace fui = freeink::ui;
static bool denyAllocations = false;
void* operator new(std::size_t n) {
  if (denyAllocations) throw std::bad_alloc();
  if (auto* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
struct Rect {
  int x = 0, y = 0, width = 480, height = 800;
};
struct Renderer {
  int getScreenWidth() { return 480; }
  int getScreenHeight() { return 800; }
};
struct Metrics {
  int verticalSpacing = 4;
};
struct UITheme {
  static UITheme& getInstance() {
    static UITheme t;
    return t;
  }
  Metrics getMetrics() { return {}; }
  Rect getScreenSafeArea(Renderer&, bool, bool) { return {}; }
};
struct TouchHeaderBackButton {
  static int contentTop(Renderer&, int, int) { return 30; }
};
struct TestTheme {
  fui::TextStyle bodyText{};
};
struct Screen {
  int seen = 0;
  TestTheme t{};
  void spacer(int16_t) {}
  void centeredText(const char*, const fui::TextStyle&) {}
  const TestTheme& theme() { return t; }
  fui::Rect body() { return {0, 30, 480, 700}; }
  void list(const fui::ListProps& p) {
    for (unsigned i = p.topIndex; i < p.count && seen < 8; ++i) {
      fui::ListItem row;
      if (p.rowProvider)
        p.rowProvider(p.rowProviderCtx, i, row);
      else
        row = p.items[i];
      if (!row.value || !row.label) std::abort();
      ++seen;
    }
  }
};
struct FilenameUiFontScope {
  FilenameUiFontScope(int, Renderer&) {}
};
void setUiContentMargin(Screen&, Renderer&, fui::Insets) {}
enum class UiListRowType { WithSubtitle };
int configureUiList(fui::ListProps&, const TestTheme&, fui::Rect, UiListRowType) { return 8; }
int scrollListBy(int top, int, int, int) { return top; }
const char* tr(int) { return "Unknown"; }
constexpr int STR_UNKNOWN_CHAPTER = 1, STR_NO_BOOKMARKS = 2;
constexpr int ACTION_DELETE = 2, ACTION_ROW = 1;
struct Swipe {
  fui::ListRevealAction reveal(int) { return {}; }
};
struct EpubReaderBookmarkListActivity {
  struct UiApp {
    using ScreenType = Screen;
  };
  const std::vector<Bookmark>& bookmarks;
  char rowProgress[16]{};
  Renderer renderer;
  int mappedInput = 0, uiTarget = 0, selectedIndex = 0, topIndex = 0, visibleRows = 1;
  Swipe swipeDelete;
  static void provideRow(void*, uint16_t, fui::ListItem&);
  void buildListScreen(Screen&);
};
'''
main = r'''
int main() {
  for (const size_t count : {0u, 1u, 1024u}) {
    std::vector<Bookmark> bookmarks(count);
    for (auto& b : bookmarks) {
      b.progress = 0.5f;
      std::snprintf(b.chapterTitle, sizeof b.chapterTitle, "Chapter");
    }
    EpubReaderBookmarkListActivity activity{bookmarks};
    // Test the first page and the tail; item indexes remain absolute.
    for (const int top : {0, count > 8 ? static_cast<int>(count - 8) : 0}) {
      activity.topIndex = top;
      denyAllocations = true;
      try {
        Screen screen;
        activity.buildListScreen(screen);
        if (screen.seen != static_cast<int>(std::min(count, size_t{8}))) std::abort();
      } catch (const std::bad_alloc&) {
        return 10;
      }
      denyAllocations = false;
    }
  }
  std::puts("PASS: empty, single and 1024-bookmark row building requires no heap allocation");
}
'''
source = (root / source_path).read_text()
production = function(source, 'void EpubReaderBookmarkListActivity::provideRow(') + '\n'
production += function(source, 'void EpubReaderBookmarkListActivity::buildListScreen(')
with tempfile.TemporaryDirectory(prefix='bookmark-memory-') as tmp:
    tmp = Path(tmp)
    cpp = tmp / 'test.cpp'
    cpp.write_text(prefix + production + main)
    binary = tmp / 'test'
    subprocess.run(['c++', '-std=c++20', '-fsanitize=address,undefined', '-I' + str(root / 'src'), '-I' + str(root / 'lib/Memory'),
                    '-I' + str(root / 'freeink-sdk/libs/ui/FreeInkUI/include'),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
