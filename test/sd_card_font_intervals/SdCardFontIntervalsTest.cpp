#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "SdCardFontManager.h"
#include "SdCardFontRegistry.h"
#include "util/WordSelectNavigator.h"
static_assert(sizeof(WordSelectNavigator::WordInfo) == 44, "C3 word records must retain their memory footprint");
#define private public
#include "SdCardFont.h"
#undef private

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t) const {
  for (const auto& file : files)
    if (file.pointSize == size) return &file;
  return nullptr;
}
const SdCardFontFileInfo* SdCardFontFamilyInfo::findClosestFile(uint8_t size, uint8_t) const {
  const SdCardFontFileInfo* best = nullptr;
  for (const auto& file : files)
    if (!best || std::abs(int(file.pointSize) - int(size)) < std::abs(int(best->pointSize) - int(size))) best = &file;
  return best;
}

// Intercept arrays only, with no allocating bookkeeping, to inject each resident-table OOM
// and verify owner/alias cleanup. Both new[] variants and deletes use the same backing allocator.
namespace {
struct Allocation {
  void* ptr;
  size_t bytes;
};
Allocation arrays[128]{};
int failArrayCall = -1, arrayCalls = 0;
size_t largestArrayRequest = 0;
void* allocateArray(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  for (auto& a : arrays)
    if (!a.ptr) {
      a = {p, n};
      return p;
    }
  std::abort();
}
size_t arrayBytes() {
  size_t bytes = 0;
  for (const auto& a : arrays)
    if (a.ptr) bytes += a.bytes;
  return bytes;
}
void freeArray(void* p) {
  if (!p) return;
  for (auto& a : arrays)
    if (a.ptr == p) {
      a = {};
      std::free(p);
      return;
    }
  std::abort();
}
}  // namespace
void* operator new[](size_t n) { return allocateArray(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  largestArrayRequest = std::max(largestArrayRequest, n);
  if (arrayCalls++ == failArrayCall) return nullptr;
  return allocateArray(n);
}
void operator delete[](void* p) noexcept { freeArray(p); }
void operator delete[](void* p, size_t) noexcept { freeArray(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { freeArray(p); }

namespace {
using Table = std::vector<EpdUnicodeInterval>;
struct Style {
  uint8_t id;
  Table intervals;
  uint32_t extraGlyphs = 0;
};
uint32_t glyphCount(const Table& t) {
  uint32_t count = 0;
  for (const auto& iv : t) count += iv.last - iv.first + 1;
  return count;
}
Table table(std::initializer_list<std::pair<uint32_t, uint32_t>> ranges) {
  Table out;
  uint32_t offset = 0;
  for (auto r : ranges) {
    out.push_back({r.first, r.second, offset});
    offset += r.second - r.first + 1;
  }
  return out;
}
uint16_t advance(uint32_t index, uint8_t style) {
  return static_cast<uint16_t>((index * 17 + 9 + style * 100) % 65535 + 1);
}
void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) bytes[at + i] = n >> (8 * i);
}
void append(std::vector<uint8_t>& bytes, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  bytes.insert(bytes.end(), p, p + n);
}
void fixture(const std::string& path, const std::vector<Style>& styles) {
  std::vector<uint8_t> bytes(32 + styles.size() * 32, 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  bytes[8] = CPFONT_VERSION;
  bytes[12] = styles.size();
  for (size_t i = 0; i < styles.size(); ++i) {
    const auto& s = styles[i];
    size_t toc = 32 + i * 32;
    bytes[toc] = s.id;
    put32(bytes, toc + 4, s.intervals.size());
    uint32_t count = glyphCount(s.intervals) + s.extraGlyphs;
    put32(bytes, toc + 8, count);
    bytes[toc + 12] = 20;
    put32(bytes, toc + 24, bytes.size());
    append(bytes, s.intervals.data(), s.intervals.size() * sizeof(EpdUnicodeInterval));
    for (uint32_t g = 0; g < count; ++g) {
      EpdGlyph glyph{};
      glyph.advanceX = advance(g, s.id);
      glyph.width = glyph.height = 1;
      glyph.dataLength = 1;
      append(bytes, &glyph, sizeof(glyph));
    }
    bytes.push_back(0x80);
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
int32_t reference(const Table& intervals, uint32_t cp) {
  for (const auto& iv : intervals)
    if (cp >= iv.first && cp <= iv.last) return iv.offset + cp - iv.first;
  return -1;
}
void verify(SdCardFont& font, uint8_t id, const Table& intervals, uint32_t compact) {
  const auto& s = font.styles_[id];
  assert(s.bmpIntervalCount == compact);
  assert(bool(s.bmpIntervals) == (compact != 0));
  assert(bool(s.fullIntervals) == (compact != intervals.size()));
  // Exhaustively compare all Unicode codepoints, including misses on both sides of the split.
  for (uint32_t cp = 0; cp <= 0x110000; ++cp) {
    const auto expected = reference(intervals, cp);
    assert(font.findGlobalGlyphIndex(s, cp) == expected);
    assert(font.getEpdFont(id)->hasCodepoint(cp) == (expected >= 0));
  }
  for (const auto& iv : intervals)
    for (uint32_t cp : {iv.first, iv.last}) {
      uint16_t actual = 0;
      assert(font.readAdvance(cp, id, &actual));
      assert(actual == advance(reference(intervals, cp), id));
    }
  assert(font.findGlobalGlyphIndex(s, UINT32_MAX) == -1);
}
void clean(const SdCardFont& f) {
  assert(!f.loaded_ && f.styleCount() == 0 && f.contentHash() == 0);
  for (const auto& s : f.styles_) {
    assert(!s.present && !s.fullIntervals && !s.bmpIntervals && !s.intervalsShared && s.bmpIntervalCount == 0);
  }
  assert(arrayBytes() == 0 && openFiles == 0);
}
}  // namespace

// Synthetic .cpfont fixture: 320 covered BMP glyphs and two 320-entry class
// tables (five blocks). The last 65 entries repeat valid classes.
void kernFixture(const std::string& path, uint8_t classes = 255) {
  constexpr uint32_t count = 320;
  std::vector<uint8_t> bytes(64, 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  bytes[8] = CPFONT_VERSION;
  bytes[12] = 1;
  put32(bytes, 36, 2);
  put32(bytes, 40, count + 1);
  bytes[44] = 20;
  bytes[49] = bytes[51] = count & 255;
  bytes[50] = bytes[52] = count >> 8;
  bytes[53] = bytes[54] = classes;
  bytes[55] = 1;
  put32(bytes, 56, 64);
  const EpdUnicodeInterval iv{0x100, 0x100 + count - 1, 0};
  append(bytes, &iv, sizeof(iv));
  const EpdUnicodeInterval replacement{0xfffd, 0xfffd, count};
  append(bytes, &replacement, sizeof(replacement));
  for (uint32_t i = 0; i <= count; ++i) {
    EpdGlyph g{};
    g.width = g.height = g.dataLength = 1;
    g.advanceX = i + 16;
    append(bytes, &g, sizeof(g));
  }
  for (int side = 0; side < 2; ++side) {
    for (uint32_t i = 0; i < count; ++i) {
      EpdKernClassEntry e{static_cast<uint16_t>(0x100 + i), static_cast<uint8_t>(i % 255 + 1)};
      append(bytes, &e, sizeof(e));
    }
  }
  for (uint32_t l = 0; l < classes; ++l)
    for (uint32_t rr = 0; rr < classes; ++rr) bytes.push_back(static_cast<uint8_t>(int((l + rr) % 11) - 5));
  const EpdLigaturePair lig{(0x100u << 16) | 0x101u, 0x102};
  append(bytes, &lig, sizeof(lig));
  bytes.push_back(0x80);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string kernText(uint32_t first, uint32_t count) {
  std::string text;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(char(0xc0 | (cp >> 6)));
    text.push_back(char(0x80 | (cp & 63)));
  }
  return text;
}
void streamingChecks(const std::string& path) {
  kernFixture(path);
  {
    SdCardFont f;
    assert(f.load(path.c_str()));
    auto& st = f.styles_[0];
    assert(st.kernBlockIndex && !st.kernBlockIndexReady);
    const auto all = kernText(0x100, 255);
    assert(f.prewarm(all.c_str(), 1, false, false) == 0);  // dictionary: ligatures, no classes
    assert(!st.kernBlockIndexReady);
    assert(f.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
    assert(f.getEpdFont()->getKerning(0x100, 0x101) == 0);
    const auto subset = kernText(0x100, 2);
    assert(f.prewarm(subset.c_str(), 1, false, true) == 0);
    assert(st.miniKernBuilt && st.miniKernLeftClassCount == 255 && st.miniKernRightClassCount == 255);
    for (uint32_t l = 0; l < 255; ++l)
      for (uint32_t rr = 0; rr < 255; ++rr)
        assert(f.getEpdFont()->getKerning(0x100 + l, 0x100 + rr) == int((l + rr) % 11) - 5);
    const int reads = readCalls;
    for (int i = 0; i < 500; ++i) {
      f.clearCache();
      assert(f.prewarm(subset.c_str(), 1, false, false) == 0);
      assert(f.getEpdFont()->getKerning(0x100, 0x101) == 0);
      assert(f.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
      assert(f.prewarm(all.c_str(), 1, false, true) == 0);
      assert(f.getEpdFont()->getKerning(0x100, 0x101) == -4);
    }
    assert(readCalls == reads);
    f.releaseForLowMemory();
    assert(st.kernBlockIndexReady && !st.miniKernBuilt);
    readCalls = 0;
    assert(f.prewarm(subset.c_str(), 1) == 0);
    const int indexedReads = readCalls;
    f.releaseForLowMemory();
    st.kernBlockIndexReady = false;
    readCalls = 0;
    assert(f.prewarm(subset.c_str(), 1) == 0);
    assert(readCalls == indexedReads + 8);  // skip four of five blocks, on both sides
    std::printf(
        "Synthetic kern fixture: full class payload=1920 B; resident index=24 B; reduction=1896 B/style; indexed reads "
        "saved=8\n");
    f.freeAll();
    clean(f);
  }
  // Fail each allocation and read during a kern top-up, then retry. Ligatures
  // have already loaded through the dictionary path and must always survive.
  int allocations = 0, reads = 0;
  {
    SdCardFont f;
    assert(f.load(path.c_str()));
    auto text = kernText(0x100, 255);
    assert(f.prewarm(text.c_str(), 1, false, false) == 0);
    arrayCalls = readCalls = 0;
    assert(f.prewarm(text.c_str(), 1) == 0);
    allocations = arrayCalls;
    reads = readCalls;
  }
  for (int mode = 0; mode < 2; ++mode) {
    for (int n = 0; n < (mode ? reads : allocations); ++n) {
      SdCardFont f;
      assert(f.load(path.c_str()));
      auto text = kernText(0x100, 255);
      assert(f.prewarm(text.c_str(), 1, false, false) == 0);
      arrayCalls = readCalls = 0;
      if (mode)
        failReadCall = n;
      else
        failArrayCall = n;
      f.prewarm(text.c_str(), 1);
      failReadCall = failArrayCall = -1;
      assert(openFiles == 0);
      assert(f.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
      assert(f.prewarm(text.c_str(), 1) == 0);
      assert(f.getEpdFont()->getKerning(0x100, 0x101) == -4);
      f.freeAll();
      clean(f);
    }
  }
  // Out-of-range class IDs are unkerned even when an indexed block is reused.
  kernFixture(path, 2);
  {
    SdCardFont f;
    assert(f.load(path.c_str()));
    auto text = kernText(0x100, 4);
    assert(f.prewarm(text.c_str(), 1) == 0);
    assert(f.getEpdFont()->getKerning(0x100, 0x101) == -4);
    assert(f.getEpdFont()->getKerning(0x102, 0x101) == 0);
  }
  assert(arrayBytes() == 0);

  // CrossInk's active-text refresh must survive the upstream merge port.
  fixture(path, {{0, table({{0x100, 0x100 + 999}})}});
  {
    SdCardFont f;
    assert(f.load(path.c_str()));
    for (uint32_t first : {0x200u, 0x100u, 0x300u, 0x180u}) {
      auto text = kernText(first, 256);
      assert(f.buildAdvanceTable(text.c_str(), 1) == 0);
      assert(f.advanceTableSize_[0] == 256 && f.advanceTableCapacity_[0] == 256);
      for (uint32_t cp = first; cp < first + 256; ++cp) assert(f.getAdvance(cp, 0) == advance(cp - 0x100, 0));
    }
    f.clearPersistentCache();
    assert(f.ensureAdvanceTableCapacity(0, 256));
    assert(!f.hasAdvanceTable());
    std::vector<SdCardFont::AdvanceEntry> expected;
    for (uint32_t block : {3u, 0u, 5u, 1u, 4u, 2u}) {
      SdCardFont::AdvanceEntry entries[50];
      for (uint32_t i = 0; i < 50; ++i) entries[i] = {block * 50 + i, uint16_t(block * 50 + i + 1)};
      const int calls = arrayCalls;
      f.mergeIntoAdvanceTable(0, entries, 50);
      assert(arrayCalls == calls);  // no temporary merge allocation with capacity available
      expected.insert(expected.end(), entries, entries + 50);
      std::sort(expected.begin(), expected.end(), [](auto a, auto b) { return a.codepoint < b.codepoint; });
      if (expected.size() > 256) expected.resize(256);
      for (size_t i = 0; i < expected.size(); ++i)
        assert(f.getAdvance(expected[i].codepoint, 0) == expected[i].advanceX);
    }
    std::puts(
        "Advance merge: 0 array allocations with spare capacity; removed up to 2048 B temporary merge payload (host "
        "fixture)");
  }
  assert(arrayBytes() == 0 && openFiles == 0);
}

void incrementalChecks(const std::string& path) {
  fixture(path, {{0, table({{'A', 'D'}, {0xFFFD, 0xFFFD}})}});
  enforceSingleReader = true;
  int allocations = 0, reads = 0, seeks = 0;
  {
    SdCardFont font;
    assert(font.load(path.c_str()));
    assert(font.prewarm("ABC", 1) == 0);
    arrayCalls = readCalls = seekCalls = 0;
    assert(font.prewarm("ABCD", 1) == 0);
    allocations = arrayCalls;
    reads = readCalls;
    seeks = seekCalls;
    assert(reads == 2);  // Only D metadata and bitmap, not all five residents.
    const auto* data = font.getEpdFont()->data;
    for (uint32_t cp : {uint32_t('A'), uint32_t('B'), uint32_t('C'), uint32_t('D'), 0xFFFDu}) {
      const auto* glyph = font.getEpdFont()->findGlyph(cp);
      assert(glyph && data->bitmap[glyph->dataOffset] == 0x80);
      assert(glyph->advanceX == advance(cp == 0xFFFD ? 4 : cp - 'A', 0));
    }
    assert(font.prewarm("DCBA", 1) == 0 && readCalls == reads);
    std::puts("Incremental cpfont: ABC -> ABCD reads only D (2 reads); repeated ABCD needs 0 reads");
  }
  // Fail every array allocation and file read in an extension. The published
  // metadata/pixels and their addresses must remain intact until a retry succeeds.
  for (int mode = 0; mode < 4; ++mode) {
    for (int n = 0; n < (mode == 0 ? allocations : mode == 1 ? reads : mode == 2 ? seeks : 1); ++n) {
      SdCardFont font;
      assert(font.load(path.c_str()));
      assert(font.prewarm("ABC", 1) == 0);
      const auto* before = font.getEpdFont()->data;
      const auto* glyphs = before->glyph;
      const auto* pixels = before->bitmap;
      const size_t bytes = arrayBytes();
      arrayCalls = readCalls = seekCalls = 0;
      if (mode == 0)
        failArrayCall = n;
      else if (mode == 1)
        failReadCall = n;
      else if (mode == 2)
        failSeekCall = n;
      else
        failOpen = true;
      assert(font.prewarm("ABCD", 1) != 0 && font.lastPrewarmFailed());
      failReadCall = failArrayCall = failSeekCall = -1;
      failOpen = false;
      assert(openFiles == 0 && arrayBytes() == bytes);
      assert(font.getEpdFont()->data == before && before->glyph == glyphs && before->bitmap == pixels);
      const auto* glyph = font.getEpdFont()->findGlyph('A');
      assert(glyph && before->bitmap[glyph->dataOffset] == 0x80);
      assert(font.prewarm("ABCD", 1) == 0 && !font.lastPrewarmFailed());
    }
  }
  // The union's kerning coverage must include the new characters; disabling
  // kerning for dictionary text must not discard ligatures or reader coverage.
  kernFixture(path);
  {
    SdCardFont font;
    assert(font.load(path.c_str()));
    assert(font.prewarm(kernText(0x100, 1).c_str(), 1) == 0);
    assert(font.prewarm(kernText(0x101, 1).c_str(), 1, false, false) == 0);
    assert(font.getEpdFont()->getKerning(0x100, 0x101) == 0);
    assert(font.prewarm(kernText(0x100, 3).c_str(), 1) == 0);
    assert(font.getEpdFont()->getKerning(0x100, 0x101) == -4);
    assert(font.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
    const int after = readCalls;
    assert(font.prewarm(kernText(0x100, 3).c_str(), 1) == 0 && readCalls == after);
  }
  assert(arrayBytes() == 0 && openFiles == 0);
  enforceSingleReader = false;
}

void contentPreparationChecks(const std::string& path) {
  constexpr size_t limit = 12U * 1024U;
  kernFixture(path);
  {
    SdCardFont font;
    assert(font.load(path.c_str(), limit));
    const EpdFontFamily family(font.getEpdFont());
    const auto text = kernText(0x100, 3);
    const auto* ligatures = font.styles_[0].ligaturePairs;
    assert(ligatures && font.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
    int expectedWidth = 0, expectedHeight = 0;
    {
      SdCardFont::MetricsScope scope(&font);
      family.getTextDimensions(text.c_str(), &expectedWidth, &expectedHeight);
    }
    const int allocationCalls = arrayCalls;
    // Section builds and reader exit release all caches. Ligature measurement
    // must remain identical before and after the visible page is prewarmed.
    for (bool preserve : {false, true}) {
      font.releaseForLowMemory(preserve);
      font.clearCache();
      font.clearPersistentCache();
      assert(font.styles_[0].ligaturePairs == ligatures);
      assert(font.getEpdFont()->getLigature(0x100, 0x101) == 0x102);
      assert(font.buildAdvanceTable(text.c_str(), 1) == 0);
      const uint32_t cps[] = {0x100, 0x101, 0x102};
      assert(font.buildAdvanceTableForCodepoints(cps, 3, false, false, 1) == 0);
      for (bool prewarm : {false, true}) {
        if (prewarm) assert(font.prewarm(text.c_str(), 1) == 0);
        int width = 0, height = 0;
        SdCardFont::MetricsScope scope(&font);
        family.getTextDimensions(text.c_str(), &width, &height);
        assert(width == expectedWidth && height == expectedHeight);
      }
      assert(arrayCalls == allocationCalls);  // Resident tables survive, no reload churn.
    }
  }
  assert(arrayBytes() == 0 && openFiles == 0);

  fixture(path, {{0, table({{'A', 'Z'}})}, {1, table({{'A', 'Z'}})}});
  const size_t budget = SdCardFont::contentResidentBytes(path.c_str(), limit);
  const size_t remaining = budget - sizeof(SdCardFont);
  constexpr size_t freeReserve = 96U * 1024U, blockReserve = 48U * 1024U;
  {
    SdCardFont font;
    const int allocationCalls = arrayCalls;
    freeHeap = freeReserve + remaining - 1;
    assert(!font.load(path.c_str(), limit, freeReserve, blockReserve));
    clean(font);
    assert(arrayCalls == allocationCalls);  // Reject after inspection, before resident arrays.
    freeHeap = freeReserve + remaining;
    maxAllocHeap = blockReserve + remaining - 1;
    assert(!font.load(path.c_str(), limit, freeReserve, blockReserve));
    clean(font);
    assert(arrayCalls == allocationCalls);
    maxAllocHeap = blockReserve + remaining;
    assert(font.load(path.c_str(), limit, freeReserve, blockReserve));
  }
  freeHeap = maxAllocHeap = 1024 * 1024;
  assert(arrayBytes() == 0 && openFiles == 0);
  // Moving admission into the loader must retain the preflight's malformed-TOC checks.
  for (bool duplicate : {false, true}) {
    fixture(path, {{0, table({{'A', 'Z'}})}, {1, table({{'A', 'Z'}})}});
    {
      std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
      if (duplicate) {
        out.seekp(64);
        out.put(0);
      } else {
        const uint32_t overflowingOffset = UINT32_MAX - 1;
        out.seekp(32 + 24);
        out.write(reinterpret_cast<const char*>(&overflowingOffset), sizeof(overflowingOffset));
      }
    }
    SdCardFont font;
    assert(!font.load(path.c_str(), limit, freeReserve, blockReserve));
    clean(font);
  }
}

void compactContentBudgetChecks(const std::string& path) {
  Table intervals;
  for (uint32_t i = 0; i < 400; ++i) intervals.push_back({0x100 + i * 2, 0x100 + i * 2, i});
  fixture(path, {{0, intervals}, {1, intervals}, {2, intervals}, {3, intervals}});
  const size_t expected = sizeof(SdCardFont) + SdCardFont::CONTENT_METRICS_CACHE_BYTES + 400 * 6 + 1;
  assert(expected <= 12U * 1024U);
  assert(SdCardFont::contentResidentBytes(path.c_str(), expected) == expected);
  {
    SdCardFont font;
    assert(font.load(path.c_str(), expected));
    assert(arrayBytes() + sizeof(font) == expected);
    for (uint8_t i = 0; i < 4; ++i) {
      uint16_t width = 0;
      assert(font.readAdvance(0x100, i, &width) && width == advance(0, i));
    }
    assert(font.styles_[3].intervalsShared);
  }
  assert(arrayBytes() == 0 && openFiles == 0);
}

void sharedContentScratchChecks(const std::string& path) {
  // Two distinguishable glyphs larger than the bitmap-cache slot limit ensure
  // the fallback scratch is used, even if bitmap caching is enabled later.
  const auto second = path + "-scratch";
  for (const auto& file : {path, second}) {
    fixture(file, {{0, table({{'A', 'A'}})}});
    std::fstream out(file, std::ios::in | std::ios::out | std::ios::binary);
    EpdGlyph glyph{};
    glyph.width = 64;
    glyph.height = 128;
    glyph.dataLength = 1024;
    glyph.advanceX = 64 * 16;
    out.seekp(64 + sizeof(EpdUnicodeInterval));
    out.write(reinterpret_cast<const char*>(&glyph), sizeof(glyph));
    const std::string bitmap(1024, file == path ? char(0xAA) : char(0x55));
    out.write(bitmap.data(), bitmap.size());
  }
  {
    SdCardFont owner, borrower;
    assert(owner.load(path.c_str(), 12U * 1024U));
    assert(borrower.load(second.c_str(), 12U * 1024U));
    const size_t before = arrayBytes();
    borrower.shareContentScratchWith(owner);
    assert(arrayBytes() == before - 1024);
    const int allocations = arrayCalls;
    for (int i = 0; i < 5; ++i) {
      const auto* a = owner.getEpdFont()->getGlyph('A');
      assert(a && owner.getOverflowBitmap(a)[0] == 0xAA);
      const auto* b = borrower.getEpdFont()->getGlyph('A');
      assert(b && borrower.getOverflowBitmap(b)[0] == 0x55);
      assert(owner.getOverflowBitmap(a) == nullptr);  // Never return the other size's pixels.
      const auto* again = owner.getEpdFont()->getGlyph('A');
      assert(again && owner.getOverflowBitmap(again)[0] == 0xAA);
      assert(borrower.getOverflowBitmap(b) == nullptr);
      failReadCall = readCalls + 1;  // Fail the next bitmap read after metadata.
      assert(borrower.getEpdFont()->getGlyph('A') == nullptr);
      failReadCall = -1;
      assert(owner.getOverflowBitmap(again) == nullptr);
    }
    assert(arrayCalls == allocations);
    // Teardown may delete the owning size first; borrowers never dereference it.
    owner.freeAll();
    borrower.freeAll();
  }
  assert(arrayBytes() == 0 && openFiles == 0);
  std::filesystem::remove(second);
}

// Distinct bytes and metrics for every key, including the same Unicode in two styles.
void contentCacheFixture(const std::string& path) {
  std::vector<uint8_t> bytes(96, 0);
  std::memcpy(bytes.data(), "CPFONT\0\0", 8);
  bytes[8] = CPFONT_VERSION;
  bytes[12] = 2;
  for (uint8_t style = 0; style < 2; ++style) {
    const size_t toc = 32 + style * 32;
    bytes[toc] = style;
    put32(bytes, toc + 4, 1);
    put32(bytes, toc + 8, 256);
    bytes[toc + 12] = 20;
    put32(bytes, toc + 24, bytes.size());
    const EpdUnicodeInterval interval{0, 255, 0};
    append(bytes, &interval, sizeof(interval));
    std::vector<uint8_t> pixels;
    for (uint32_t cp = 0; cp < 256; ++cp) {
      EpdGlyph glyph{};
      glyph.advanceX = advance(cp, style);
      glyph.width = cp == 0 ? 0 : cp == 1 ? 64 : cp == 2 ? 24 : 8;
      glyph.height = cp == 0 ? 0 : cp == 1 ? 64 : cp == 2 ? 171 : 2;
      glyph.dataLength = cp == 0 ? 0 : cp == 1 ? 512 : cp == 2 ? 513 : 2;
      glyph.dataOffset = pixels.size();
      append(bytes, &glyph, sizeof(glyph));
      for (uint32_t j = 0; j < glyph.dataLength; ++j) pixels.push_back(uint8_t(cp + style * 71 + j));
    }
    append(bytes, pixels.data(), pixels.size());
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
void bodyMetricsCacheSaturationChecks(const std::string& path) {
  contentCacheFixture(path);
  SdCardFont font;
  assert(font.load(path.c_str()));
  auto hash = [](uint32_t cp, uint8_t style) { return ((cp ^ (uint32_t(style) << 21)) * 2654435761U) >> 25; };
  uint32_t hot = 0, miss = 0;
  for (uint32_t cp = 3; cp < 131 && !hot; ++cp) {
    if (hash(cp, 0) == 0) continue;
    for (uint32_t other = 131; other < 256; ++other) {
      if (hash(cp, 0) != hash(other, 0)) continue;
      hot = cp;
      miss = other;
      break;
    }
  }
  assert(hot && miss);
  SdCardFont::MetricsScope scope(&font);
  auto measure = [&](uint32_t cp, uint8_t style = 0) {
    const auto* glyph = font.getEpdFont(style)->getGlyph(cp);
    assert(glyph && glyph->advanceX == advance(cp, style));
  };
  measure(hot);
  for (uint32_t cp = 3; cp < 131; ++cp) measure(cp);
  const int warmReads = readCalls;
  measure(miss);
  measure(hot);
  assert(readCalls - warmReads == 1);  // A colliding cold glyph must not evict the hot one.
  for (int pass = 0; pass < 4; ++pass) {
    for (uint32_t cp = 3; cp < 256; ++cp) {
      measure(cp, pass % 2);  // Repeated full scans wrap the clock; the style remains part of the key.
      measure(hot);
    }
  }
  font.releaseForLowMemory(true);
  assert(!font.contentMetrics_);
  const int coldReads = readCalls;
  measure(hot);
  measure(hot);
  assert(readCalls - coldReads == 1);
}

void contentCacheCollisionChecks(const std::string& path) {
  contentCacheFixture(path);
  SdCardFont font;
  assert(font.load(path.c_str(), 12U * 1024U));
  font.prepareContentBitmaps();
  assert(font.contentBitmapCapacity_ >= 512);
  const auto allocationCount = arrayCalls;
  const auto residentBytes = arrayBytes();
  auto draw = [&](uint32_t cp, uint8_t style = 0) {
    const auto* glyph = font.getEpdFont(style)->getGlyph(cp);
    assert(glyph && glyph->advanceX == advance(cp, style));
    const auto* bitmap = font.getOverflowBitmap(glyph);
    assert(bitmap);
    for (uint32_t i = 0; i < glyph->dataLength; ++i) assert(bitmap[i] == uint8_t(cp + style * 71 + i));
    return glyph;
  };
  auto hash = [](uint32_t cp, uint8_t style) { return ((cp ^ (uint32_t(style) << 21)) * 2654435761U) >> 25; };
  uint32_t a = 3, b = 4;
  while (b < 256 && hash(a, 0) != hash(b, 0)) ++b;
  assert(b < 256);
  const auto* first = draw(a);
  const auto* second = draw(b);
  assert(first != second);
  const int warmReads = readCalls;
  for (int pass = 0; pass < 100; ++pass) {
    assert(draw(a) == first && draw(b) == second);
  }
  assert(readCalls == warmReads);
  // Same codepoint in another resolved style has independent pixels/metrics.
  draw(a, 1);
  const int styledReads = readCalls;
  for (int pass = 0; pass < 100; ++pass) {
    draw(a);
    draw(a, 1);
    draw(b);
  }
  assert(readCalls == styledReads);
  // A vacancy BEFORE an existing key must not hide it or create a duplicate.
  auto& vacant = font.contentMetrics_[font.contentMetricsIndex(first)];
  vacant = {};
  const int holeReads = readCalls;
  assert(draw(b) == second && readCalls == holeReads);
  // A metrics-only flood must prefer bitmapless entries, retaining resident b.
  for (uint32_t cp = 3; cp < 256; ++cp) {
    uint16_t width;
    assert(font.readAdvance(cp, 1, &width) && width == advance(cp, 1));
  }
  const int floodReads = readCalls;
  assert(draw(b) == second && readCalls == floodReads);
  assert(arrayCalls == allocationCount && arrayBytes() == residentBytes);
  // Full-table churn remains correct, including failure before replacing a victim.
  font.clearPersistentCache();
  for (uint32_t cp = 3; cp < 131; ++cp) draw(cp);
  failReadCall = readCalls;
  assert(!font.contentGlyphMetrics(200, 0, 200));
  failReadCall = -1;
  const int failureReads = readCalls;
  for (uint32_t cp = 3; cp < 131; ++cp) draw(cp);
  assert(readCalls == failureReads);
  for (int pass = 0; pass < 3; ++pass)
    for (uint32_t cp = 3; cp < 200; ++cp) draw(cp, pass % 2);
  // Exact ring boundaries, adjacency and wrap: only overwritten ranges expire.
  font.clearPersistentCache();
  font.contentBitmapCapacity_ = 8;  // Keep allocation, restrict usable ring for test.
  const auto* x = draw(10);
  const auto* y = draw(11);
  draw(12);
  draw(13);
  assert(font.contentBitmapNext_ == 8);
  draw(14);
  assert(font.contentBitmapNext_ == 2 && !font.getOverflowBitmap(x));
  const int adjacentReads = readCalls;
  assert(draw(11) == y && readCalls == adjacentReads);
  // A zero-byte glyph neither advances the cursor nor evicts an adjacent bitmap.
  draw(0);
  const int spaceReads = readCalls;
  draw(0);
  assert(font.contentBitmapNext_ == 2 && readCalls == spaceReads);
  assert(draw(11) == y && readCalls == spaceReads);
  // Read failure invalidates the overlapped owner before any retry.
  font.contentGlyphMetrics(15, 0, 15);
  failReadCall = readCalls;
  assert(!font.getEpdFont()->getGlyph(15));
  failReadCall = -1;
  assert(!font.getOverflowBitmap(y));
  draw(15);
  draw(11);
  // 512 bytes admitted exactly; 513 uses the existing scratch fallback.
  font.clearPersistentCache();
  font.contentBitmapCapacity_ = 512;
  const auto* edge = draw(1);
  assert(font.contentMetricsIndex(edge) >= 0 && font.contentBitmapNext_ == 512);
  const auto* oversized = draw(2);
  assert(font.contentMetricsIndex(oversized) < 0);
  assert(font.contentBitmapNext_ == 512);
  assert(draw(1) == edge);
  font.clearPersistentCache();
  font.contentBitmapCapacity_ = 1;
  assert(font.contentMetricsIndex(draw(3)) < 0);
  font.releaseForLowMemory();
  assert(!font.contentBitmaps_);
  font.freeAll();
  clean(font);
}

void contentSizeChecks(const std::string& path) {
  fixture(path, {{0, table({{0x41, 0x5A}})}, {1, table({{0x41, 0x5A}})}});
  constexpr size_t limit = 12U * 1024U;
  const size_t budget =
      sizeof(SdCardFont) + SdCardFont::CONTENT_METRICS_CACHE_BYTES + sizeof(SdCardFont::PerStyle::BmpInterval16) + 1;
  const auto arraysBefore = arrayBytes();
  readCalls = 0;
  assert(SdCardFont::contentResidentBytes(path.c_str(), limit) == budget);
  const int inspectionReads = readCalls;
  assert(arrayBytes() == arraysBefore && openFiles == 0);
  assert(SdCardFont::contentResidentBytes(path.c_str(), budget - 1) == 0);
  // Every preflight read failure closes the file and allocates nothing.
  for (int failure = 0; failure < inspectionReads; ++failure) {
    readCalls = 0;
    failReadCall = failure;
    assert(SdCardFont::contentResidentBytes(path.c_str(), limit) == 0);
    failReadCall = -1;
    assert(arrayBytes() == arraysBefore && openFiles == 0);
  }
  {
    SdCardFont font;
    assert(!font.load(path.c_str(), budget - 1));
    clean(font);  // Reject before any interval or bitmap allocation.
    assert(font.load(path.c_str(), budget));
    assert(font.contentBitmapBytes_ == 1);
    assert(arrayBytes() + sizeof(font) <= budget);
    const auto baseline = arrayBytes();
    const auto allocationCalls = arrayCalls;
    const uint32_t cps[] = {0x41, 0x42};
    assert(font.buildAdvanceTable("ABCD", 3) == 0);
    assert(font.buildAdvanceTableForCodepoints(cps, 2, false, false, 3) == 0);
    assert(!font.hasAdvanceTable());
    // CSS can make almost the entire book use an optional size. Repeated width
    // measurement must reuse its bounded cache, without paragraph allocations.
    readCalls = 0;
    for (unsigned pass = 0; pass < 100; ++pass) {
      for (uint8_t style = 0; style < 2; ++style) {
        for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) {
          uint16_t width = 0;
          assert(font.readAdvance(cp, style, &width));
          assert(width == advance(cp - 'A', style));
        }
      }
      font.clearCache();
    }
    assert(readCalls < 700);  // 5,200 lookups; collisions may reread, never mix styles.
    assert(arrayBytes() == baseline && arrayCalls == allocationCalls && openFiles == 0);
    const EpdFontFamily measuredFamily(font.getEpdFont(), font.getEpdFont(1));
    int expectedWidth[2]{}, expectedHeight[2]{};
    for (uint8_t style = 0; style < 2; ++style)
      measuredFamily.getTextDimensions("ABCDEFGHIJKLMNOPQRSTUVWXYZ", &expectedWidth[style], &expectedHeight[style],
                                       static_cast<EpdFontFamily::Style>(style));
    font.clearPersistentCache();
    readCalls = 0;
    {
      SdCardFont::MetricsScope outer(&font);
      for (unsigned pass = 0; pass < 100; ++pass) {
        SdCardFont::MetricsScope nested(&font);
        for (uint8_t style = 0; style < 2; ++style) {
          int width = 0, height = 0;
          measuredFamily.getTextDimensions("ABCDEFGHIJKLMNOPQRSTUVWXYZ", &width, &height,
                                           static_cast<EpdFontFamily::Style>(style));
          assert(width == expectedWidth[style] && height == expectedHeight[style]);
        }
      }
    }
    assert(font.metricsDepth_ == 0 && readCalls < 700);
    assert(arrayBytes() == baseline && arrayCalls == allocationCalls && openFiles == 0);
    // Metrics-only glyphs must never stand in for the bitmap when drawing resumes.
    const auto* drawn = font.getEpdFont()->getGlyph('A');
    assert(drawn && font.getOverflowBitmap(drawn)[0] == 0x80);
    font.clearPersistentCache();
    uint16_t width = 0;
    const int coldReads = readCalls;
    assert(font.readAdvance('A', 0, &width) && readCalls == coldReads + 1);
    assert(font.readAdvance('A', 0, &width) && readCalls == coldReads + 1);
    font.releaseForLowMemory();
    failReadCall = readCalls;
    assert(!font.readAdvance('A', 0, &width));
    failReadCall = -1;
    assert(font.readAdvance('A', 0, &width) && width == advance(0, 0));
    const int recoveredReads = readCalls;
    assert(font.readAdvance('A', 0, &width) && readCalls == recoveredReads);
    assert(font.prewarm("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 3) == 0);
    const EpdFontFamily family(font.getEpdFont(), font.getEpdFont(1));
    assert(family.getFallbackCodepoint('A', EpdFontFamily::REGULAR) == 'A');
    assert(family.findGlyphData('B', EpdFontFamily::BOLD).glyph);
    assert(!family.findGlyphData('?', EpdFontFamily::REGULAR).glyph);
    for (unsigned pass = 0; pass < 100; ++pass) {
      for (uint32_t cp = 0x41; cp <= 0x5A; ++cp) {
        const auto* glyph = font.getEpdFont(pass % 2)->getGlyph(cp);
        assert(glyph && font.getOverflowBitmap(glyph)[0] == 0x80);
        assert(glyph->advanceX == advance(cp - 0x41, pass % 2));
      }
      font.clearCache();
    }
    assert(arrayBytes() == baseline && arrayCalls == allocationCalls);
    font.releaseForLowMemory();
    assert(font.prewarm("AB", 3) == 0);
    const auto* glyph = font.getEpdFont()->getGlyph(0x41);
    assert(glyph && font.getOverflowBitmap(glyph)[0] == 0x80);
    font.freeAll();
    clean(font);
  }
  // Scratch, width-cache and interval OOM must release all earlier allocations.
  for (int failure = 0; failure < 3; ++failure) {
    SdCardFont font;
    arrayCalls = 0;
    failArrayCall = failure;
    assert(!font.load(path.c_str(), limit));
    failArrayCall = -1;
    clean(font);
  }
  // A file changed after load must not overrun its now-smaller scratch.
  {
    SdCardFont font;
    assert(font.load(path.c_str(), limit) && font.contentBitmapBytes_ == 1);
    {
      std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
      EpdGlyph larger{};
      larger.width = 9;
      larger.height = 1;
      larger.dataLength = 2;  // Valid 1-bit packing, larger than the loaded scratch.
      out.seekp(96 + sizeof(EpdUnicodeInterval));
      out.write(reinterpret_cast<const char*>(&larger), sizeof(larger));
    }
    assert(font.getEpdFont()->getGlyph(0x41) == nullptr);
    font.freeAll();
    clean(font);
    fixture(path, {{0, table({{0x41, 0x5A}})}, {1, table({{0x41, 0x5A}})}});
  }
  // A supported file whose glyphs cannot fit scratch falls back before registration.
  {
    std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
    const uint16_t tooLarge = 4097;
    out.seekp(96 + sizeof(EpdUnicodeInterval) + offsetof(EpdGlyph, dataLength));
    out.write(reinterpret_cast<const char*>(&tooLarge), sizeof(tooLarge));
  }
  {
    SdCardFont font;
    assert(SdCardFont::contentResidentBytes(path.c_str(), limit) == 0);
    assert(!font.load(path.c_str(), limit));
    clean(font);
  }
  SdCardFontFamilyInfo family;
  family.name = "Content";
  for (const uint8_t size : {8, 10, 12, 14, 16, 24}) {
    const std::string file = path + std::to_string(size);
    fixture(file, {{0, table({{0x41, 0x5A}})}});
    family.files.push_back({file, size, 0});
  }
  GfxRenderer renderer;
  SdCardFontManager manager;
  // Manager admission must read each selected file exactly once. The prior
  // separate preflight added a second scan of every optional glyph table.
  int expectedReads = 0;
  for (const uint8_t size : {12, 24, 8}) {
    SdCardFont font;
    readCalls = 0;
    assert(font.load(family.findFile(size)->path.c_str(), size == 12 ? 0 : limit));
    expectedReads += readCalls;
  }
  readCalls = 0;
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(readCalls == expectedReads);
  const int highId = manager.getFontId(family.name);
  size_t scratchOwners = 0, scratchBorrowers = 0;
  for (const auto& [id, font] : renderer.getSdCardFonts()) {
    if (!font->leanContent_) continue;
    scratchOwners += bool(font->contentBitmap_);
    scratchBorrowers += bool(font->contentScratchOwner_);
  }
  assert(scratchOwners == 1 && scratchBorrowers == 1);
  assert(renderer.getFontMap().size() == 3 && renderer.getSdCardFonts().size() == 3);
  for (const auto& [id, font] : renderer.getFontMap()) assert(font.sizeFamilyId() == highId);
  // Reordered/rounded CSS hints request the same installed sizes. A low-heap
  // reentry must reuse this live profile instead of rerunning admission.
  assert(manager.setContentSizeScales({192, 512}));
  const int reads = readCalls;
  freeHeap = 97 * 1024;
  maxAllocHeap = 49 * 1024;
  assert(manager.matchesContentSizeRequests(family));
  assert(manager.getFontId(family.name) == highId && readCalls == reads);
  freeHeap = maxAllocHeap = 1024 * 1024;
  manager.unloadAll(renderer);
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == highId);  // Same set, opposite load order.
  assert(manager.setContentSizeScales({191, 513}));
  assert(manager.matchesContentSizeRequests(family));
  manager.setContentSizeScales({341, 299});
  assert(!manager.matchesContentSizeRequests(family));
  manager.setContentSizeScales({512, 192});
  manager.unloadAll(renderer);
  assert(renderer.getFontMap().empty() && renderer.getSdCardFonts().empty());
  assert(arrayBytes() == 0);
  freeHeap = 97 * 1024;  // Enough for body, below reserve plus actual optional bytes.
  assert(manager.loadFamilyClosest(family, renderer, 12));
  const int lowId = manager.getFontId(family.name);
  assert(lowId != highId && renderer.getFontMap().size() == 1);
  assert(manager.matchesContentSizeRequests(family));  // Remembers skipped requests too.
  manager.setContentSizeScales({192, 512});
  freeHeap = 1024 * 1024;
  assert(manager.matchesContentSizeRequests(family) && manager.getFontId(family.name) == lowId);
  manager.setContentSizeScales({512, 192});
  manager.unloadAll(renderer);
  freeHeap = 1024 * 1024;
  maxAllocHeap = 49 * 1024;  // Fragmented heap must also decline optional sizes.
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == lowId && renderer.getFontMap().size() == 1);
  manager.unloadAll(renderer);
  maxAllocHeap = 1024 * 1024;
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == highId);
  manager.unloadAll(renderer);
  // Real byte costs admit small fonts that the old fixed 12KB allowance
  // rejected, while still leaving the full 96KB/48KB reserves.
  freeHeap = 110 * 1024;
  maxAllocHeap = 62 * 1024;
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == highId && renderer.getFontMap().size() == 3);
  const size_t optionalBytes = arrayBytes() - sizeof(SdCardFont::PerStyle::BmpInterval16) +
                               2 * (sizeof(SdCardFont) + 512);  // Objects and registration overhead.
  assert(freeHeap - optionalBytes >= 96 * 1024 && maxAllocHeap - optionalBytes >= 48 * 1024);
  manager.unloadAll(renderer);
  freeHeap = maxAllocHeap = 1024 * 1024;
  // Simulate another allocation consuming headroom during the optional load.
  // Body has one compact interval; extra scratch/tables trigger the drop.
  freeHeapProbe = []() -> uint32_t {
    return arrayBytes() > sizeof(SdCardFont::PerStyle::BmpInterval16) ? 95U * 1024U : freeHeap;
  };
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == lowId && renderer.getFontMap().size() == 1);
  manager.unloadAll(renderer);
  freeHeapProbe = nullptr;
  // Book-aware content choices remain limited to two budgeted font objects.
  // Changing the preferences changes the identity, and a dictionary-style
  // unload/reload must restore that same identity and actual size set.
  assert(manager.setContentSizeScales({341, 299}));
  assert(!manager.matchesContentSizeRequests(family));
  assert(!manager.setContentSizeScales({341, 299}));
  assert((manager.contentSizeTargetsForBody(12) == std::array<uint8_t, 2>{16, 14}));
  assert(manager.loadFamilyClosest(family, renderer, 12));
  const int bookId = manager.getFontId(family.name);
  assert(bookId != highId && renderer.getFontMap().size() == 3);
  for (const auto& [id, font] : renderer.getFontMap()) {
    assert(font.sizeFamilyId() == bookId);
    assert(font.pointSize() == 12 || font.pointSize() == 14 || font.pointSize() == 16);
  }
  manager.unloadAll(renderer);
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) == bookId);
  manager.unloadAll(renderer);
  freeHeap = 97 * 1024;
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(renderer.getFontMap().size() == 1 && manager.getFontId(family.name) == lowId);
  manager.unloadAll(renderer);
  freeHeap = 1024 * 1024;
  assert(manager.loadFamilyClosest(family, renderer, 10));
  assert(renderer.getFontMap().size() == 3);
  // An even-only family rounds the 13pt growth target up, consistently with
  // the renderer, while retaining the exact 12pt subheading size.
  for (const auto& [id, font] : renderer.getFontMap())
    assert(font.pointSize() == 10 || font.pointSize() == 12 || font.pointSize() == 14);
  manager.unloadAll(renderer);
  manager.setContentSizeScales({512, 192});
  // Variant replacement must invalidate the body identity even at the same size.
  {
    std::fstream out(family.files.back().path, std::ios::in | std::ios::out | std::ios::binary);
    out.seekp(32 + 12);
    out.put(21);
  }
  assert(manager.loadFamilyClosest(family, renderer, 12));
  assert(manager.getFontId(family.name) != highId);
  manager.unloadAll(renderer);
  assert(arrayBytes() == 0 && openFiles == 0);
  // A tied but unsupported 45pt file must not displace supported 43pt growth.
  for (const uint8_t size : {22, 43, 45}) {
    const std::string file = path + std::to_string(size);
    fixture(file, {{0, table({{0x41, 0x5A}})}});
    family.files.push_back({file, size, 0});
  }
  assert(manager.loadFamilyClosest(family, renderer, 22));
  bool found43 = false;
  for (const auto& [id, font] : renderer.getFontMap()) {
    assert(font.pointSize() != 45);
    found43 |= font.pointSize() == 43;
  }
  assert(found43);
  manager.unloadAll(renderer);
  assert(arrayBytes() == 0 && openFiles == 0);
  for (const auto& file : family.files) std::filesystem::remove(file.path);
}

void upstreamFontTests(const std::string& path) {
  // Corrupt length, out-of-file bitmap, and 32-bit offset wrap must all be blank
  // through direct metrics, prewarm, and on-demand/lean-content loading.
  for (int damage = 0; damage < 3; ++damage) {
    fixture(path, {{0, table({{0x41, 0x44}})}});
    constexpr size_t glyphOffset = 64 + sizeof(EpdUnicodeInterval) + sizeof(EpdGlyph);
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    EpdGlyph glyph{};
    file.seekg(glyphOffset);
    file.read(reinterpret_cast<char*>(&glyph), sizeof(glyph));
    if (damage == 0) glyph.dataLength = 500;
    if (damage == 1) glyph.dataOffset = 10000;
    if (damage == 2) glyph.dataOffset = UINT32_MAX;
    file.seekp(glyphOffset);
    file.write(reinterpret_cast<const char*>(&glyph), sizeof(glyph));
    file.close();
    for (bool lean : {false, true}) {
      SdCardFont::takeCorruptGlyphNotice();
      SdCardFont font;
      assert(font.load(path.c_str(), lean ? 100000 : 0));
      uint16_t width = 99;
      assert(font.readAdvance('B', 0, &width) && width == 0);
      assert(SdCardFont::takeCorruptGlyphNotice());
      assert(!SdCardFont::takeCorruptGlyphNotice());
      assert(font.buildAdvanceTable("ABCD", 1) == 0);
      assert(font.prewarm("ABCD", 1) == (lean ? 0 : 1));  // fixture has no replacement glyph
      const auto* bad = font.getEpdFont()->getGlyph('B');
      assert(bad && bad->width == 0 && bad->dataLength == 0 && bad->advanceX == 0);
      const auto* good = font.getEpdFont()->getGlyph('A');
      assert(good && good->dataLength == 1 && good->width == 1);
      assert(!SdCardFont::takeCorruptGlyphNotice());
      font.clearPersistentCache();
      bad = font.getEpdFont()->getGlyph('B');
      assert(bad && bad->dataLength == 0);
    }
  }
  fixture(path, {{0, table({{0x100, 0x100 + 999}})}, {1, table({{0x100, 0x100 + 999}})}});
  {
    SdCardFont f;
    assert(f.load(path.c_str()));
    // Descending input forces replacement of the largest collected entry.
    std::string text;
    for (int i = 999; i >= 0; --i) text += kernText(0x100 + i, 1);
    largestArrayRequest = 0;
    assert(f.buildAdvanceTable(text.c_str(), 3) == 0);
    assert(largestArrayRequest <= (256 + 2) * sizeof(SdCardFont::AdvanceEntry));
    for (uint32_t cp = 0x100; cp < 0x200; ++cp) {
      assert(f.getAdvance(cp, 0) == advance(cp - 0x100, 0));
      assert(f.getAdvance(cp, 1) == advance(cp - 0x100, 1));
    }
    const auto cached = kernText(0x100, 256);
    const int before = arrayCalls, readsBefore = readCalls;
    assert(f.buildAdvanceTable(cached.c_str(), 3) == 0);
    assert(arrayCalls == before && readCalls == readsBefore);
    // Different styles already hold different ranges; collect independently.
    assert(f.buildAdvanceTable(kernText(0x300, 256).c_str(), 2) == 0);
    assert(f.buildAdvanceTable(cached.c_str(), 3) == 0);
    assert(f.getAdvance(0x100, 1) == advance(0, 1));
    f.clearPersistentCache();
    // Missing low codepoints must not displace valid higher codepoints.
    text = kernText(0x80, 128) + cached;
    assert(f.buildAdvanceTable(text.c_str(), 1) == 128);
    assert(f.advanceTableSize_[0] == 256);
    assert(f.getAdvance(0x1ff, 0) == advance(255, 0));
    f.clearPersistentCache();
    const std::deque<std::string> emptyWords{"", ""};
    assert(f.buildAdvanceTable(emptyWords, true, 1) == 2);
    assert(f.advanceTableSize_[0] == 0);
    // Failure of bounded paragraph scratch is recoverable.
    assert(f.ensureAdvanceTableCapacity(0, 256));
    failArrayCall = arrayCalls;
    assert(f.buildAdvanceTable(cached.c_str(), 1) == -1);
    failArrayCall = -1;
    assert(f.buildAdvanceTable(cached.c_str(), 1) == 0);
  }
  assert(arrayBytes() == 0 && openFiles == 0);
}

int main() {
  auto dir = std::filesystem::temp_directory_path() / "crossink-font-interval-tests";
  std::filesystem::create_directories(dir);
  const auto path = (dir / "test.cpfont").string();
  upstreamFontTests(path);
  const auto bmp = table({{0, 2}, {0x41, 0x44}, {0xFFFD, 0xFFFF}});
  const auto mixed =
      table({{0x41, 0x44}, {0x4E00, 0x4E02}, {0xFFFD, 0xFFFF}, {0x10000, 0x10002}, {0x10FFFF, 0x10FFFF}});
  const auto crossing = table({{0x41, 0x42}, {0xFFFE, 0x10002}, {0x20000, 0x20000}});
  const auto supplementary = table({{0x10000, 0x10001}, {0x10FFFF, 0x10FFFF}});
  const auto fullBmp = table({{0, 0xFFFE}, {0xFFFF, 0xFFFF}});  // offset 65535; total glyphs 65536
  const auto maxGlyphsMixed = table({{0, 0xFFFE}, {0x10000, 0x10000}});
  for (auto item :
       {std::pair{bmp, 3u}, {mixed, 3u}, {crossing, 1u}, {supplementary, 0u}, {fullBmp, 2u}, {maxGlyphsMixed, 1u}}) {
    fixture(path, {{0, item.first}});
    SdCardFont font;
    assert(font.load(path.c_str()));
    verify(font, 0, item.first, item.second);
    size_t expected = item.second * 6 + (item.first.size() - item.second) * 12;
    assert(arrayBytes() == expected);
    font.clearCache();
    font.releaseForLowMemory();
    assert(arrayBytes() == expected);
    verify(font, 0, item.first, item.second);
    font.freeAll();
    clean(font);
  }

  // Maximum interval count: sparse BMP prefix plus one supplementary singleton.
  Table sparse;
  sparse.reserve(4096);
  for (uint32_t i = 0; i < 4095; ++i) sparse.push_back({i * 2, i * 2, i});
  sparse.push_back({0x20000, 0x20000, 4095});
  fixture(path, {{0, sparse}, {1, sparse}, {2, sparse}, {3, sparse}});
  {
    SdCardFont font;
    assert(font.load(path.c_str()));
    assert(font.styles_[0].bmpIntervalCount == 4095);
    assert(arrayBytes() == 4095 * 6 + 12);
    for (uint8_t id = 0; id < 4; ++id) {
      if (id) assert(font.styles_[id].intervalsShared);
      for (uint32_t cp = 0; cp < 8192; ++cp)
        assert(font.findGlobalGlyphIndex(font.styles_[id], cp) == reference(sparse, cp));
      assert(font.findGlobalGlyphIndex(font.styles_[id], 0x20000) == 4095);
    }
    std::printf("Sparse fixture: 1 shared table: old=49152 B new=%zu B saved=24570 B (table payload only)\n",
                arrayBytes());
  }
  assert(arrayBytes() == 0);

  // Four styles: two share mixed coverage, one differs in the full suffix, one in the compact prefix.
  auto differentFull = mixed;
  differentFull.back().first = differentFull.back().last = 0x10FFFE;
  auto differentBmp = mixed;
  differentBmp.front().first++;
  differentBmp.front().last++;
  fixture(path, {{3, differentBmp}, {1, mixed, 1}, {2, differentFull}, {0, mixed}});  // out-of-order TOC
  {
    SdCardFont font;
    assert(font.load(path.c_str()));
    assert(font.styles_[1].intervalsShared);
    assert(font.styles_[1].bmpIntervals == font.styles_[0].bmpIntervals);
    assert(font.styles_[1].fullIntervals == font.styles_[0].fullIntervals);
    assert(!font.styles_[2].intervalsShared && !font.styles_[3].intervalsShared);
    verify(font, 0, mixed, 3);
    verify(font, 1, mixed, 3);
    verify(font, 2, differentFull, 3);
    verify(font, 3, differentBmp, 3);
    assert(arrayBytes() == 3 * (3 * 6 + 2 * 12));
    std::printf(
        "Mixed fixture: 3 distinct tables x (3 BMP + 2 full): old=180 B new=%zu B saved=54 B (table payload only)\n",
        arrayBytes());
    assert(font.prewarm("A\xF0\x90\x80\x80", 0x03, true) == 0);
    assert(font.getEpdFont(0)->getGlyph(0x10000)->advanceX == advance(reference(mixed, 0x10000), 0));
    const uint32_t cps[] = {0x41, 0x10000};
    assert(font.buildAdvanceTableForCodepoints(cps, 2, false, false, 0x03) == 0);
    assert(font.getAdvance(0x10000, 1) == advance(reference(mixed, 0x10000), 1));
    font.freeAll();
    clean(font);
  }
  // Fail every split-table allocation, including after an alias and during a later distinct table.
  for (int failure = 0; failure < 6; ++failure) {
    SdCardFont font;
    assert(font.load(path.c_str()));  // A failed replacement load must also release the previous font.
    arrayCalls = 0;
    failArrayCall = failure;
    assert(!font.load(path.c_str()));
    failArrayCall = -1;
    clean(font);
    assert(font.load(path.c_str()));
    font.freeAll();
    clean(font);
  }
  // Fail every read in load (header, TOC, validation, compact conversion, full suffix) and retry.
  readCalls = 0;
  {
    SdCardFont font;
    assert(font.load(path.c_str()));
  }
  int loadReads = readCalls;
  for (int failure = 0; failure < loadReads; ++failure) {
    SdCardFont font;
    assert(font.load(path.c_str()));
    readCalls = 0;
    failReadCall = failure;
    assert(!font.load(path.c_str()));
    failReadCall = -1;
    clean(font);
    assert(font.load(path.c_str()));
    font.freeAll();
    clean(font);
  }
  // Malformed later style after an earlier mixed table is shared must free its single owner.
  auto invalid = mixed;
  invalid.back().offset++;
  fixture(path, {{0, mixed}, {1, mixed}, {2, invalid}});
  {
    SdCardFont font;
    assert(!font.load(path.c_str()));
    clean(font);
    fixture(path, {{0, bmp}});
    assert(font.load(path.c_str()));
  }
  assert(arrayBytes() == 0 && openFiles == 0);
  for (auto invalidTable : {table({{0, 0x10000}}), table({{0x41, 0x41}})}) {
    // Reject glyph counts above 65536 and offsets above 65535 rather than narrowing.
    if (invalidTable[0].last == 0x41) invalidTable[0].offset = 65536;
    fixture(path, {{0, invalidTable}});
    SdCardFont font;
    assert(!font.load(path.c_str()));
    clean(font);
  }
  streamingChecks(path);
  incrementalChecks(path);
  contentPreparationChecks(path);
  contentSizeChecks(path);
  contentCacheFixture(path);
  {
    SdCardFont body;
    assert(body.load(path.c_str()));
    const int before = readCalls;
    {
      SdCardFont::MetricsScope metrics(&body);
      for (int row = 0; row < 100; ++row) {
        for (unsigned char cp = 'a'; cp <= 'z'; ++cp) {
          const auto* glyph = body.getEpdFont()->getGlyph(cp);
          assert(glyph && glyph->advanceX == advance(cp, 0));
        }
      }
    }
    // Cold metadata reads only. The same workload read 5,200 records on RC-3.
    assert(readCalls - before == 26);
    const auto* glyph = body.getEpdFont()->getGlyph('z');
    assert(glyph && body.getOverflowBitmap(glyph)[0] == 'z');
    body.releaseForLowMemory(true);
    assert(!body.contentMetrics_);

    failArrayCall = arrayCalls;
    {
      SdCardFont::MetricsScope metrics(&body);
      const int startReads = readCalls;
      const int startAllocations = arrayCalls;
      const auto* measured = body.getEpdFont()->getGlyph('a');
      assert(measured && measured->advanceX == advance('a', 0));
      assert(!body.contentMetrics_);
      assert(!body.getOverflowBitmap(measured));
      assert(body.getEpdFont()->getGlyph('a') == measured);
      assert(readCalls == startReads + 1);         // One metadata record, no pixels.
      assert(arrayCalls == startAllocations + 1);  // Only the failed optional-cache allocation.
    }
    failArrayCall = -1;
    const auto* drawn = body.getEpdFont()->getGlyph('a');
    assert(drawn && body.getOverflowBitmap(drawn)[0] == 'a');
  }
  bodyMetricsCacheSaturationChecks(path);
  contentCacheCollisionChecks(path);
  compactContentBudgetChecks(path);
  sharedContentScratchChecks(path);
  std::filesystem::remove_all(dir);
  std::puts("SD font interval regression checks passed");
}
