// Indexed class streaming and in-place advance merging adapted from serialx,
// CrossPoint #3831 (9610da70), including the correctness fixes in #3838.
#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PoolBudget.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "ComplexShaper.h"
#include "EpdFontFamily.h"
#include "FlashBlobCache.h"
#include "Fnv1a.h"
#include "ShapingTokens.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;
constexpr uint16_t CONTENT_BITMAP_BYTES = 4096;
constexpr uint32_t MAX_INTERVALS = 4096;
constexpr uint32_t MAX_GLYPHS = 65536;
constexpr uint32_t MAX_KERN_ENTRIES = 4096;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool inspectContentGlyphs(HalFile& file, const uint32_t offset, const uint32_t count, uint16_t& bitmapBytes) {
  if (uint64_t(offset) + uint64_t(count) * sizeof(EpdGlyph) > file.fileSize() || !file.seekSet(offset)) {
    LOG_ERR("SDCF", "Cannot inspect content-size glyphs");
    return false;
  }
  // 128-byte batches keep SD reads small without one read per glyph.
  constexpr uint32_t BATCH_GLYPHS = 8;
  EpdGlyph glyphs[BATCH_GLYPHS];
  for (uint32_t i = 0; i < count;) {
    const uint32_t batch = std::min(count - i, BATCH_GLYPHS);
    const size_t bytes = batch * sizeof(EpdGlyph);
    if (file.read(glyphs, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SDCF", "Cannot read content-size glyphs");
      return false;
    }
    for (uint32_t j = 0; j < batch; ++j) {
      if (glyphs[j].dataLength > CONTENT_BITMAP_BYTES) {
        LOG_ERR("SDCF", "Content-size glyph exceeds scratch");
        return false;
      }
      bitmapBytes = std::max(bitmapBytes, glyphs[j].dataLength);
    }
    i += batch;
  }
  return true;
}

// Allocation-free description of the compact interval table for one style.
struct ContentIntervals {
  uint32_t offset = 0;
  uint32_t count = 0;
  uint32_t glyphs = 0;
  uint16_t bmpCount = 0;
};

bool sameContentIntervals(HalFile& file, const ContentIntervals& a, const ContentIntervals& b, bool& same) {
  same = false;
  if (a.count != b.count) return true;
  // Two 96-byte batches compare on-disk records exactly, without opening a
  // second reader or allocating the tables that admission is trying to budget.
  EpdUnicodeInterval left[8], right[8];
  for (uint32_t i = 0; i < a.count; i += 8) {
    const size_t bytes = std::min<uint32_t>(8, a.count - i) * sizeof(EpdUnicodeInterval);
    if (!file.seekSet(a.offset + i * sizeof(EpdUnicodeInterval)) || file.read(left, bytes) != int(bytes) ||
        !file.seekSet(b.offset + i * sizeof(EpdUnicodeInterval)) || file.read(right, bytes) != int(bytes)) {
      LOG_ERR("SDCF", "Cannot compare content-size intervals");
      return false;
    }
    if (memcmp(left, right, bytes) != 0) return true;
  }
  same = true;
  return true;
}

bool inspectContentIntervals(HalFile& file, ContentIntervals* tables, const uint8_t index, size_t& bytes) {
  auto& table = tables[index];
  if (uint64_t(table.offset) + uint64_t(table.count) * sizeof(EpdUnicodeInterval) > file.fileSize() ||
      !file.seekSet(table.offset)) {
    LOG_ERR("SDCF", "Invalid content-size interval extent");
    return false;
  }
  uint32_t expected = 0, previous = 0;
  for (uint32_t i = 0; i < table.count; ++i) {
    EpdUnicodeInterval iv{};
    if (file.read(&iv, sizeof(iv)) != sizeof(iv) || iv.first > iv.last || (i && iv.first <= previous) ||
        uint64_t(iv.last) - iv.first + 1 > table.glyphs || iv.offset != expected ||
        uint64_t(iv.offset) + iv.last - iv.first + 1 > table.glyphs) {
      LOG_ERR("SDCF", "Invalid content-size interval");
      return false;
    }
    if (i == table.bmpCount && iv.last <= UINT16_MAX && iv.offset <= UINT16_MAX) ++table.bmpCount;
    expected += iv.last - iv.first + 1;
    previous = iv.last;
  }
  for (uint8_t i = 0; i < index; ++i) {
    bool same = false;
    if (!sameContentIntervals(file, table, tables[i], same)) return false;
    if (same) {
      bytes = 0;
      return true;
    }
  }
  bytes = size_t(table.bmpCount) * 6 + size_t(table.count - table.bmpCount) * sizeof(EpdUnicodeInterval);
  return true;
}

// Adapted from CrossPoint #3948. Keep only the lowest entries a capped merge
// can retain. The caller provides two extra slots for injected space/hyphen.
class LowestCodepoints {
 public:
  LowestCodepoints(uint32_t* buffer, uint32_t limit) : buffer_(buffer), limit_(limit) {}
  void add(uint32_t cp) {
    if (limit_ == 0 || (count_ == limit_ && cp >= buffer_[largest_])) return;
    if (std::find(buffer_, buffer_ + count_, cp) != buffer_ + count_) return;
    if (count_ < limit_) {
      buffer_[count_++] = cp;
      if (count_ < limit_) return;
    } else {
      buffer_[largest_] = cp;
    }
    largest_ = std::max_element(buffer_, buffer_ + count_) - buffer_;
  }
  void addReserved(uint32_t cp) {
    if (std::find(buffer_, buffer_ + count_, cp) == buffer_ + count_) buffer_[count_++] = cp;
  }
  uint32_t size() const { return count_; }

 private:
  uint32_t* buffer_;
  uint32_t limit_;
  uint32_t count_ = 0;
  uint32_t largest_ = 0;
};

uint32_t countUtf8Codepoints(const char* text, const uint32_t limit) {
  if (!text) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t count = 0;
  while (*p && count < limit) {
    if (utf8NextCodepoint(&p) == 0) break;
    ++count;
  }
  return count;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40U * 1024U;
constexpr size_t MINI_RETAIN_MIN_MAX_ALLOC_HEAP = 32U * 1024U;
constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;

template <typename T, typename Capacity>
bool ensureArrayCapacity(T*& buffer, Capacity& capacity, const uint32_t needed) {
  if (buffer && capacity >= needed) return true;
  delete[] buffer;
  buffer = new (std::nothrow) T[needed > 0 ? needed : 1];
  capacity = buffer ? static_cast<Capacity>(needed) : 0;
  return buffer != nullptr;
}

}  // namespace

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

namespace {
HeapByteBuffer allocateMiniBitmap(const uint32_t needed, const bool preserveInternalReserve) {
  const size_t bytes = needed > 0 ? needed : 1;
  HeapByteBuffer bitmap;
  if (psramHeapAvailable()) {
    if (MemoryBudget::canAllocatePsram(bytes)) bitmap = makePsramByteBufferNoThrow(bytes);
    if (!bitmap && MemoryBudget::canAllocateInternal(bytes, MemoryBudget::EPUB_FONT_INTERNAL_RESERVE,
                                                     MINI_RETAIN_MIN_MAX_ALLOC_HEAP)) {
      bitmap = makeInternalByteBufferNoThrow(bytes);
    }
  } else if (!preserveInternalReserve ||
             (bytes <= ESP.getFreeHeap() && MemoryBudget::EPUB_FONT_INTERNAL_RESERVE <= ESP.getFreeHeap() - bytes &&
              bytes <= ESP.getMaxAllocHeap())) {
    bitmap = makeDefaultByteBufferNoThrow(bytes);
  }
  return bitmap;
}
}  // namespace

bool SdCardFont::ensureBitmapCapacity(PerStyle& s, const uint32_t needed) {
  if (s.miniBitmap && s.miniBitmapCapacity >= needed) return true;
  // Cold rebuilds release the old payload before growing. Incremental updates
  // allocate separately so a failed read/allocation can retain the resident view.
  s.miniData.bitmap = nullptr;
  s.miniBitmap.reset();
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniBitmapPool = MemoryPool::None;
  s.miniBitmap = allocateMiniBitmap(needed, false);
  if (!s.miniBitmap) return false;
  s.miniBitmapCapacity = needed;
  s.miniBitmapPool = byteBufferPool(s.miniBitmap.get());
  return true;
}

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  s.miniBitmap.reset();
  s.miniBitmapPool = MemoryPool::None;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  s.miniMetadataOnly = false;
  s.miniHysteresisPending = false;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s) {
  if (ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < MINI_RETAIN_MIN_MAX_ALLOC_HEAP) {
    freeStyleMiniData(s);
    return;
  }

  // Underuse hysteresis, on the bitmap arena (the dominant allocation): an
  // outlier page (e.g. three styles cramped together) would otherwise pin its
  // high-water arena for the rest of the book. Keep while the page used at
  // least 3/4 of capacity; release only after several consecutive rebuilds
  // below that, so alternating dense/sparse pages never thrash. Evaluated at
  // most once per rebuild (a scope both constructs and destructs through here,
  // and subset hits load nothing new to judge).
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        LOG_DBG("SDCF", "Releasing underused mini buffers: used=%u capacity=%u", s.miniBitmapUsed,
                s.miniBitmapCapacity);
        freeStyleMiniData(s);
        return;
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }
  // Data (intervals/glyphs/bitmaps/kern) deliberately survives the scope: the
  // next prewarm subset-checks against it, which is what lets the idle prewarm
  // of page N+1 serve the actual page turn with zero SD reads.
}

void SdCardFont::freeStyleLigatures(PerStyle& s) {
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.ligaturesLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernLeftCapacity = 0;
  s.miniKernRightCapacity = 0;
  s.miniKernMatrixCapacity = 0;
  s.miniKernBuilt = false;
  // The visible view must never borrow freed mini buffers on a failed top-up.
  applyKernLigaturePointers(s, s.miniData, false);
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  // A shared table is owned by the style it was aliased from -- that style's own
  // freeStyleAll() call frees it. Deleting it here too would double-free.
  if (!s.intervalsShared) {
    delete[] s.fullIntervals;
    delete[] s.bmpIntervals;
  }
  s.fullIntervals = nullptr;
  s.bmpIntervals = nullptr;
  s.intervalsShared = false;
  s.bmpIntervalCount = 0;
  freeStyleLigatures(s);
  delete[] s.kernBlockIndex;
  s.kernBlockIndex = nullptr;
  s.kernBlockIndexReady = false;
  s.present = false;
  s.shaper.reset();
  s.shapingBlobOffset = 0;
  s.shapingBlobLength = 0;
  s.shapingBlobKey = 0;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
  contentBitmap_.reset();
  contentScratchOwner_ = nullptr;
  contentScratchUser_ = nullptr;
  contentBitmaps_.reset();
  contentBitmapCapacity_ = 0;
  contentBitmapNext_ = 0;
  contentBitmapsAttempted_ = false;
  contentMetrics_.reset();
  bodyMetricsAttempted_ = false;
  bodyMetricsClock_ = 0;
  contentBitmapBytes_ = 0;
  contentBitmapInternalLimit_ = 0;
  leanContent_ = false;
}

void SdCardFont::shareContentScratchWith(SdCardFont& owner) {
  if (&owner == this || !leanContent_ || !owner.leanContent_ || contentScratchOwner_ || owner.contentScratchOwner_ ||
      owner.contentBitmapBytes_ < contentBitmapBytes_)
    return;
  clearOverflow();
  contentBitmap_.reset();
  contentScratchOwner_ = &owner;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (!leanContent_) delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data, const bool includeKerning) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = includeKerning ? s.miniKernLeftClasses : nullptr;
  data.kernRightClasses = includeKerning ? s.miniKernRightClasses : nullptr;
  // .cpfont files map packed class tables and a dense matrix. Explicitly clear
  // the built-in-only representation because getKerning() selects by pointer.
  data.kernLeftCodepoints = nullptr;
  data.kernLeftClassIds = nullptr;
  data.kernRightCodepoints = nullptr;
  data.kernRightClassIds = nullptr;
  data.kernMatrix = includeKerning ? s.miniKernMatrix : nullptr;
  data.kernRowOffsets = nullptr;
  data.kernSparseCols = nullptr;
  data.kernSparseValues = nullptr;
  data.kernLeftEntryCount = includeKerning ? s.miniKernLeftEntryCount : 0;
  data.kernRightEntryCount = includeKerning ? s.miniKernRightEntryCount : 0;
  data.kernLeftClassCount = includeKerning ? s.miniKernLeftClassCount : 0;
  data.kernRightClassCount = includeKerning ? s.miniKernRightClassCount : 0;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.ligaturePairs ? s.header.ligaturePairCount : 0;
}

bool SdCardFont::loadStyleLigatures(PerStyle& s) {
  if (s.ligaturesLoaded) return true;
  if (s.header.ligaturePairCount == 0) {
    s.ligaturesLoaded = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for ligatures: %s", filePath_);
    return false;
  }
  s.ligaturePairs = makeUniqueNoThrow<EpdLigaturePair[]>(s.header.ligaturePairCount).release();
  if (!s.ligaturePairs) {
    LOG_ERR("SDCF", "Failed to allocate ligature pairs");
    return false;
  }
  if (!file.seekSet(s.ligatureFileOffset)) {
    LOG_ERR("SDCF", "Failed to seek to ligature data");
    freeStyleLigatures(s);
    return false;
  }
  size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
  if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
    LOG_ERR("SDCF", "Failed to read ligature pairs");
    freeStyleLigatures(s);
    return false;
  }

  s.ligaturesLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Ligatures loaded: %u", s.header.ligaturePairCount);
  return true;
}

// --- Per-page mini kern matrix ---

namespace {
// Kern class-table entries per SD read, and per block of the block index.
constexpr uint16_t KERN_CLASS_BLOCK = 64;

uint16_t kernClassBlocks(const uint16_t entryCount) { return (entryCount + KERN_CLASS_BLOCK - 1) / KERN_CLASS_BLOCK; }

// Reads the sorted kern class table at `offset` and stores the class ID of each
// sorted page codepoint in classes[i] (0 = no kerning class). `index` holds the
// first codepoint of each block followed by the table's last codepoint. Once it
// is filled, only blocks whose codepoint range holds a page codepoint are read.
// Otherwise every block is read, and `index` (if any) is filled on the way.
bool readKernClasses(HalFile& file, const uint32_t offset, const uint16_t entryCount, const uint8_t classCount,
                     const uint32_t* codepoints, const uint32_t cpCount, uint8_t* classes, EpdKernClassEntry* block,
                     uint16_t* index, const bool indexReady) {
  memset(classes, 0, cpCount);
  const uint16_t blocks = kernClassBlocks(entryCount);
  uint32_t next = 0;
  for (uint16_t b = 0; b < blocks && (next < cpCount || (index && !indexReady)); b++) {
    if (index && indexReady) {
      while (next < cpCount && codepoints[next] < index[b]) next++;
      const uint32_t end = b + 1 < blocks ? index[b + 1] : index[blocks] + 1u;
      if (next == cpCount || codepoints[next] >= static_cast<uint32_t>(index[blocks]) + 1u) break;
      if (codepoints[next] >= end) continue;
    }
    const uint16_t count = std::min<uint16_t>(KERN_CLASS_BLOCK, entryCount - b * KERN_CLASS_BLOCK);
    const int bytes = count * sizeof(EpdKernClassEntry);
    if (!file.seekSet(offset + static_cast<uint32_t>(b) * KERN_CLASS_BLOCK * sizeof(EpdKernClassEntry)) ||
        file.read(reinterpret_cast<uint8_t*>(block), bytes) != bytes) {
      return false;
    }
    if (index && !indexReady) {
      index[b] = block[0].codepoint;
      if (b + 1 == blocks) index[blocks] = block[count - 1].codepoint;
    }
    for (uint16_t i = 0; i < count && next < cpCount; i++) {
      const uint32_t cp = block[i].codepoint;
      // An ID past the matrix would index outside it; treat the entry as unkerned.
      const uint8_t classId = block[i].classId <= classCount ? block[i].classId : 0;
      while (next < cpCount && codepoints[next] < cp) next++;
      for (uint32_t j = next; j < cpCount && codepoints[j] == cp; j++) classes[j] = classId;
    }
  }
  return true;
}
}  // namespace

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
// The class tables are streamed from SD rather than kept resident, so reading
// holds no per-style kern allocation between pages.
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  // No freeStyleMiniKern here: it zeroed the capacities, which forced the
  // ensureArrayCapacity calls below to reallocate every page and defeated the
  // buffer reuse. prewarmStyle is the only caller and the success path
  // overwrites the contents and all four counts, so keeping the buffers is
  // safe. The early returns zero the counts (buffers kept) so a page with no
  // applicable kern pairs kerns as none instead of through the previous
  // page's tables.
  const auto resetMiniKernCounts = [&s]() {
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
  };
  if (s.header.kernLeftEntryCount == 0 || s.header.kernRightEntryCount == 0 || cpCount == 0) {
    resetMiniKernCounts();
    return true;  // font has no kern classes — nothing to build
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }

  // Scratch: left and right class per page codepoint, then one buffer shared by
  // the class-table blocks and the matrix rows. Freed on return.
  const uint32_t ioBytes =
      std::max<uint32_t>(KERN_CLASS_BLOCK * sizeof(EpdKernClassEntry), s.header.kernRightClassCount);
  auto scratch = makeUniqueNoThrow<uint8_t[]>(cpCount * 2 + ioBytes);
  if (!scratch) {
    LOG_ERR("SDCF", "Failed to allocate mini kern scratch (%u bytes)", cpCount * 2 + ioBytes);
    freeStyleMiniKern(s);
    return false;
  }
  uint8_t* leftClasses = scratch.get();
  uint8_t* rightClasses = leftClasses + cpCount;
  uint8_t* io = rightClasses + cpCount;
  auto* block = reinterpret_cast<EpdKernClassEntry*>(io);
  uint16_t* rightIndex =
      s.kernBlockIndex ? s.kernBlockIndex + kernClassBlocks(s.header.kernLeftEntryCount) + 1 : nullptr;
  if (!readKernClasses(file, s.kernLeftFileOffset, s.header.kernLeftEntryCount, s.header.kernLeftClassCount, codepoints,
                       cpCount, leftClasses, block, s.kernBlockIndex, s.kernBlockIndexReady) ||
      !readKernClasses(file, s.kernRightFileOffset, s.header.kernRightEntryCount, s.header.kernRightClassCount,
                       codepoints, cpCount, rightClasses, block, rightIndex, s.kernBlockIndexReady)) {
    LOG_ERR("SDCF", "Failed to read kern classes");
    freeStyleMiniKern(s);
    return false;
  }
  s.kernBlockIndexReady = s.kernBlockIndex != nullptr;

  // Step 1: mark used left/right classes and count the page's kerned codepoints.
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (leftClasses[i]) {
      usedLeft[leftClasses[i]] = true;
      miniLeftCount++;
    }
    if (rightClasses[i]) {
      usedRight[rightClasses[i]] = true;
      miniRightCount++;
    }
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint16_t numLeft = 0, numRight = 0;  // up to 255: a uint8_t loop counter would never end
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = static_cast<uint8_t>(numLeft);
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = static_cast<uint8_t>(numRight);
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    resetMiniKernCounts();
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: size the three mini buffers (reused across pages when they fit; the
  // per-page sizes vary by a few entries, which as free+realloc churn was punching
  // non-coalescing holes in the heap every page turn).
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  if (!ensureArrayCapacity(s.miniKernLeftClasses, s.miniKernLeftCapacity, miniLeftCount) ||
      !ensureArrayCapacity(s.miniKernRightClasses, s.miniKernRightCapacity, miniRightCount) ||
      !ensureArrayCapacity(s.miniKernMatrix, s.miniKernMatrixCapacity, matrixBytes)) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 4: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    const auto cp = static_cast<uint16_t>(codepoints[i]);  // classes are only set for BMP codepoints
    if (leftClasses[i]) {
      s.miniKernLeftClasses[lIdx].codepoint = cp;
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[leftClasses[i]];
      lIdx++;
    }
    if (rightClasses[i]) {
      s.miniKernRightClasses[rIdx].codepoint = cp;
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rightClasses[i]];
      rIdx++;
    }
  }

  // Step 5: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata).
  const auto* row = reinterpret_cast<const int8_t*>(io);
  for (uint16_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(io, s.header.kernRightClassCount) != static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint16_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = row[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = static_cast<uint8_t>(numLeft);
  s.miniKernRightClassCount = static_cast<uint8_t>(numRight);

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.coverageHandler = &SdCardFont::onCoverageQuery;
  s.stubData.shapeHandler = s.shaper ? &SdCardFont::onShape : nullptr;
}

bool SdCardFont::onCoverageQuery(void* ctx, const uint32_t codepoint) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return false;  // coverage index freed/never loaded
  return octx->self->findGlobalGlyphIndex(s, codepoint) >= 0;
}

// --- Shaping section ---

namespace {
constexpr char SHAPING_MAGIC[4] = {'C', 'P', 'S', 'H'};
constexpr uint16_t SHAPING_VERSION = 2;  // 2: the layout font carries its shaping plans (CPpl)
// magic(4) version(2) reserved(2) ppem26_6(4) blobLength(4) blobHash(4)
constexpr uint32_t SHAPING_HEADER_SIZE = 20;
// An Indic layout font is 5-80 KB; anything far larger is not one.
constexpr uint32_t SHAPING_MAX_BLOB = 256 * 1024;
}  // namespace

bool SdCardFont::loadShapingSection(HalFile& file, const uint8_t styleIdx, const uint32_t sectionOffset) {
  uint8_t header[SHAPING_HEADER_SIZE];
  if (!file.seekSet(sectionOffset) || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("SDCF", "Style %u: unreadable shaping section at %u", styleIdx, sectionOffset);
    return false;
  }
  const uint16_t version = readU16(header + 4);
  const uint32_t ppem26_6 = readU32(header + 8);
  const uint32_t length = readU32(header + 12);
  // FNV-1a of the blob: every size of a family carries the same layout font,
  // and the shapers share one copy of it through this key.
  const uint32_t contentHash = readU32(header + 16);
  if (memcmp(header, SHAPING_MAGIC, sizeof(SHAPING_MAGIC)) != 0 || version != SHAPING_VERSION || ppem26_6 == 0 ||
      length == 0 || length > SHAPING_MAX_BLOB) {
    LOG_ERR("SDCF", "Style %u: unsupported shaping section (v%u, %u bytes)", styleIdx, version, length);
    return false;
  }
  // The blob's last byte must be in the file: a copy cut short would otherwise
  // fail every load of it.
  uint8_t last;
  if (uint64_t(sectionOffset) + SHAPING_HEADER_SIZE + length > file.fileSize() ||
      !file.seekSet(sectionOffset + SHAPING_HEADER_SIZE + length - 1) || file.read(&last, 1) != 1) {
    LOG_ERR("SDCF", "Style %u: shaping section truncated (%u-byte blob)", styleIdx, length);
    return false;
  }
  auto& s = styles_[styleIdx];
  s.shaper = makeUniqueNoThrow<ComplexShaper>();
  if (!s.shaper) {
    LOG_ERR("SDCF", "Style %u: OOM creating shaper", styleIdx);
    return false;
  }
  s.shapingBlobOffset = sectionOffset + SHAPING_HEADER_SIZE;
  s.shapingBlobLength = length;
  s.shapingBlobKey = contentHash | 1u;
  s.shaper->setBlobSource(&SdCardFont::loadShapingBlob, &overflowCtx_[styleIdx], s.shapingBlobKey);
  s.shaper->setScale(ppem26_6);
  return true;
}

namespace {
struct BlobFileCtx {
  HalFile* file;
  uint32_t base;
};

bool readBlobChunk(void* ctx, const uint32_t offset, uint8_t* buf, const uint32_t length) {
  const auto* c = static_cast<BlobFileCtx*>(ctx);
  return c->file->seekSet(c->base + offset) && c->file->read(buf, length) == static_cast<int>(length);
}
}  // namespace

bool SdCardFont::loadShapingBlob(void* ctx, ComplexShaper::Blob* out) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  HalFile file;
  if (!Storage.openFileForRead("SDCF", octx->self->filePath_, file)) {
    ComplexShaper::noteSourceUnavailable();
    return false;
  }
  BlobFileCtx source{&file, s.shapingBlobOffset};

  // No-PSRAM boards map the layout font from internal flash (see
  // FlashBlobCache.h); otherwise it is read into the heap, within the limit
  // ComplexShaper sets.
  if (const uint8_t* mapped = FlashBlobCache::acquire(s.shapingBlobKey, s.shapingBlobLength, readBlobChunk, &source)) {
    *out = ComplexShaper::Blob{mapped, s.shapingBlobLength, &FlashBlobCache::release};
    file.close();
    return true;
  }
  // A layout font the heap may not hold shapes only when mapped from flash.
  if (s.shapingBlobLength > ComplexShaper::memoryStats().maxHeapSource) {
    if (FlashBlobCache::canHold(s.shapingBlobKey, s.shapingBlobLength)) {
      ComplexShaper::noteSourceUnavailable();  // the flash copy may succeed later
    } else {
      LOG_ERR("SDCF", "%u-byte shaping data fits neither the heap nor a flash cache here; text is not shaped",
              s.shapingBlobLength);
    }
    file.close();
    return false;
  }
  auto* blob = static_cast<uint8_t*>(ComplexShaper::allocate(s.shapingBlobLength));
  if (!blob) {
    file.close();
    return false;
  }
  if (!readBlobChunk(&source, 0, blob, s.shapingBlobLength)) {
    LOG_ERR("SDCF", "Failed to read %u-byte shaping blob", s.shapingBlobLength);
    ComplexShaper::deallocate(blob);
    ComplexShaper::noteSourceUnavailable();
    file.close();
    return false;
  }
  file.close();
  *out = ComplexShaper::Blob{blob, s.shapingBlobLength, nullptr};
  return true;
}

bool SdCardFont::onShape(void* ctx, const char* utf8, std::string* out) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const auto& shaper = octx->self->styles_[octx->styleIdx].shaper;
  return shaper != nullptr && shaper->shape(utf8, *out);
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

size_t SdCardFont::contentResidentBytes(const char* path, const size_t limit) {
  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Cannot inspect content font: %s", path);
    return 0;
  }
  // Only one small TOC/glyph record is held at a time. Close this handle
  // before the normal loader reopens the same path on hardware.
  const size_t bytes = [&]() -> size_t {
    uint8_t header[HEADER_SIZE];
    if (file.read(header, sizeof(header)) != sizeof(header) || memcmp(header, CPFONT_MAGIC, 8) != 0 ||
        readU16(header + 8) != CPFONT_VERSION || header[12] == 0 || header[12] > MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid content-font header");
      return 0;
    }
    size_t resident = sizeof(SdCardFont) + CONTENT_METRICS_CACHE_BYTES;
    if (resident > limit) return 0;
    uint16_t bitmapBytes = 0;
    uint8_t styles = 0;
    ContentIntervals tables[MAX_STYLES]{};
    for (uint8_t i = 0; i < header[12]; ++i) {
      uint8_t toc[STYLE_TOC_ENTRY_SIZE];
      if (!file.seekSet(HEADER_SIZE + i * STYLE_TOC_ENTRY_SIZE) || file.read(toc, sizeof(toc)) != sizeof(toc) ||
          toc[0] >= MAX_STYLES || (styles & (1U << toc[0]))) {
        LOG_ERR("SDCF", "Invalid content-font style TOC");
        return 0;
      }
      styles |= 1U << toc[0];
      const uint32_t intervals = readU32(toc + 4);
      const uint32_t glyphs = readU32(toc + 8);
      if (intervals > MAX_INTERVALS || glyphs > MAX_GLYPHS || readU16(toc + 17) > MAX_KERN_ENTRIES ||
          readU16(toc + 19) > MAX_KERN_ENTRIES) {
        LOG_ERR("SDCF", "Invalid content-font counts");
        return 0;
      }
      tables[i] = {readU32(toc + 24), intervals, glyphs};
      size_t intervalBytes = 0;
      if (!inspectContentIntervals(file, tables, i, intervalBytes)) return 0;
      resident += intervalBytes + size_t(toc[23]) * sizeof(EpdLigaturePair);
      if (resident > limit) return 0;
      const uint64_t offset = uint64_t(readU32(toc + 24)) + uint64_t(intervals) * sizeof(EpdUnicodeInterval);
      if (offset > UINT32_MAX || !inspectContentGlyphs(file, static_cast<uint32_t>(offset), glyphs, bitmapBytes))
        return 0;
    }
    return bitmapBytes <= limit - resident ? resident + bitmapBytes : 0;
  }();
  file.close();
  return bytes;
}

bool SdCardFont::load(const char* path, const size_t residentLimit, const size_t freeHeapReserve,
                      const size_t maxAllocHeapReserve) {
  freeAll();
  leanContent_ = residentLimit != 0;
  size_t residentBytes = sizeof(SdCardFont);
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  fileSize_ = file.fileSize();
  corruptGlyphsLogged_ = 0;

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a::hash(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  uint32_t shapingOffsets[MAX_STYLES] = {};
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a::hash(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES || styles_[styleId].present) {
      LOG_ERR("SDCF", "Invalid or duplicate styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    if (leanContent_ &&
        uint64_t(dataOffset) + uint64_t(s.header.intervalCount) * sizeof(EpdUnicodeInterval) > UINT32_MAX) {
      LOG_ERR("SDCF", "Content-size glyph offset overflow");
      file.close();
      freeAll();
      return false;
    }
    computeStyleFileOffsets(s, dataOffset);
    // Formerly reserved: files without shaping data carry zero here.
    shapingOffsets[styleId] = readU32(tocBuf + 28);
  }

  // Count the actual compact, shared interval storage before allocating. This
  // keeps large multilingual fonts eligible without relaxing heap reserves.
  if (leanContent_) {
    residentBytes += CONTENT_METRICS_CACHE_BYTES;
    if (residentBytes > residentLimit) {
      LOG_DBG("SDCF", "Content size exceeds %zu-byte resident budget", residentLimit);
      file.close();
      freeAll();
      return false;
    }
    ContentIntervals tables[MAX_STYLES]{};
    uint8_t tableCount = 0;
    for (const auto& s : styles_) {
      if (!s.present) continue;
      tables[tableCount] = {s.intervalsFileOffset, s.header.intervalCount, s.header.glyphCount};
      size_t intervalBytes = 0;
      if (!inspectContentIntervals(file, tables, tableCount++, intervalBytes)) {
        file.close();
        freeAll();
        return false;
      }
      residentBytes += intervalBytes + size_t(s.header.ligaturePairCount) * sizeof(EpdLigaturePair);
      if (residentBytes > residentLimit) {
        LOG_DBG("SDCF", "Content size exceeds %zu-byte resident budget", residentLimit);
        file.close();
        freeAll();
        return false;
      }
      if (!inspectContentGlyphs(file, s.glyphsFileOffset, s.header.glyphCount, contentBitmapBytes_)) {
        file.close();
        freeAll();
        return false;
      }
    }
    if (contentBitmapBytes_ > residentLimit - residentBytes) {
      LOG_DBG("SDCF", "Content size exceeds %zu-byte resident budget", residentLimit);
      file.close();
      freeAll();
      return false;
    }
    contentBitmapInternalLimit_ = std::min<size_t>(8U * 1024U, residentLimit - residentBytes - contentBitmapBytes_);
    // Inspect once, then decide admission before any resident-table allocation.
    // The manager already allocated this object and separately reserves registration bytes.
    const size_t remainingBytes = residentBytes + contentBitmapBytes_ - sizeof(SdCardFont);
    if (ESP.getFreeHeap() < freeHeapReserve + remainingBytes ||
        ESP.getMaxAllocHeap() < maxAllocHeapReserve + remainingBytes) {
      LOG_DBG("SDCF", "Skipping content size: preserving heap reserve");
      file.close();
      freeAll();
      return false;
    }
    // Font-lifetime scratch (0..4KB) is reused for every glyph. Size it to
    // this file rather than reserving 4KB even for tiny glyphs.
    if (contentBitmapBytes_) contentBitmap_ = makeUniqueNoThrow<uint8_t[]>(contentBitmapBytes_);
    if (contentBitmapBytes_ && !contentBitmap_) {
      LOG_ERR("SDCF", "Cannot allocate content-size glyph scratch");
      file.close();
      freeAll();
      return false;
    }
    // Allocate once with the font: 3KB is too large for the reader task stack,
    // and each loaded size needs its own cache. Included in the resident ceiling.
    contentMetrics_ = makeUniqueNoThrow<ContentMetricsEntry[]>(CONTENT_METRICS_CACHE_SIZE);
    if (!contentMetrics_) {
      LOG_ERR("SDCF", "Cannot allocate content-size metrics cache (%zu bytes)", CONTENT_METRICS_CACHE_BYTES);
      file.close();
      freeAll();
      return false;
    }
  }
  styleCount_ = styleCount;
  contentHash_ = hash;

  // Split-table idea adapted from YACP (MIT), Totofaki's commit:
  // https://github.com/Sichroteph/YACP/commit/23568ad3580740068cd3466f400b6900f08a1a15
  // Keep eligible BMP records in a 6-byte prefix even when supplementary ranges follow.
  // CrossInk retains cross-style sharing and its existing owner/unload contract.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    uint32_t bmpCount = 0;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};

    // Regular/bold/italic weights of the same family almost always cover the identical codepoint
    // set, so a later style's table is usually a byte-for-byte copy of an earlier one's. Sharing
    // it saves a full table per style, which on a broad CJK font is tens of KB, and saves the
    // PEAK rather than just the residency: allocating first and de-duplicating afterwards still
    // needs both tables at once, and that peak is what fails on a tight heap.
    // The decision rides along with the validation read below -- every record is already being
    // read here -- so it costs no second pass over the table and no buffer to hold one.
    // A style stays a candidate only while its table has matched every record so far.
    uint8_t shareCandidates = 0;
    for (uint8_t k = 0; k < i; k++) {
      const auto& owner = styles_[k];
      if (!owner.present || owner.header.intervalCount != s.header.intervalCount) continue;
      if (!owner.bmpIntervals && !owner.fullIntervals) continue;
      shareCandidates |= static_cast<uint8_t>(1u << k);
    }
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      // A range crossing U+FFFF belongs entirely to the full-width suffix.
      if (j == bmpCount && iv.last <= UINT16_MAX && iv.offset <= UINT16_MAX) ++bmpCount;
      for (uint8_t k = 0; k < i && shareCandidates != 0; k++) {
        if ((shareCandidates & (1u << k)) == 0) continue;
        const auto& owner = styles_[k];
        const auto candidate = owner.intervalAt(j);
        const bool same = candidate.first == iv.first && candidate.last == iv.last && candidate.offset == iv.offset;
        if (!same) shareCandidates &= static_cast<uint8_t>(~(1u << k));
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    // Survived every record: alias the earlier style's table instead of allocating a copy.
    // freeStyleAll() skips delete[] when intervalsShared is set, so only the owner frees.
    for (uint8_t k = 0; k < i && shareCandidates != 0; k++) {
      if ((shareCandidates & (1u << k)) == 0) continue;
      auto& owner = styles_[k];
      s.bmpIntervals = owner.bmpIntervals;
      s.fullIntervals = owner.fullIntervals;
      s.bmpIntervalCount = owner.bmpIntervalCount;
      s.intervalsShared = true;
      LOG_DBG("SDCF", "Style %u: sharing style %u's %u-interval table (%u B not allocated)", i, k,
              s.header.intervalCount,
              bmpCount * static_cast<uint32_t>(sizeof(PerStyle::BmpInterval16)) +
                  (s.header.intervalCount - bmpCount) * static_cast<uint32_t>(sizeof(EpdUnicodeInterval)));
      break;
    }

    // Only the allocate-and-read path below needs the records again; a shared style is done.
    if (!s.intervalsShared && !file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (!s.intervalsShared) {
      s.bmpIntervalCount = static_cast<uint16_t>(bmpCount);
      const uint32_t fullCount = s.header.intervalCount - bmpCount;
      // Selected-font lifetime storage, up to 4096 * 12 bytes: too large for stack/static.
      // freeStyleAll() owns both allocations; shared styles borrow them without deleting.
      if (bmpCount > 0) {
        s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[bmpCount];
        if (!s.bmpIntervals) {
          LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
          freeAll();
          return false;
        }
        for (uint32_t j = 0; j < bmpCount; ++j) {
          if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
            LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
            freeAll();
            return false;
          }
          s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                               static_cast<uint16_t>(iv.offset)};
        }
      }
      if (fullCount > 0) {
        s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[fullCount];
        if (!s.fullIntervals) {
          LOG_ERR("SDCF", "Failed to allocate %u full intervals for style %u", fullCount, i);
          freeAll();
          return false;
        }
        const size_t intervalsBytes = fullCount * sizeof(EpdUnicodeInterval);
        if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) !=
            static_cast<int>(intervalsBytes)) {
          LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
          freeAll();
          return false;
        }
      }
      LOG_DBG(
          "SDCF", "Style %u interval RAM: compact=%u full=%u bytes=%u", i, bmpCount, fullCount,
          static_cast<unsigned>(bmpCount * sizeof(PerStyle::BmpInterval16) + fullCount * sizeof(EpdUnicodeInterval)));
    }

    if (!leanContent_ && s.header.kernLeftEntryCount && s.header.kernRightEntryCount) {
      // Optional load-lifetime index: at most 260 bytes for two uint16_t indexes
      // (load limits each class table to 4096 entries); replaces up to 24576 class bytes.
      s.kernBlockIndex = makeUniqueNoThrow<uint16_t[]>(kernClassBlocks(s.header.kernLeftEntryCount) + 1 +
                                                       kernClassBlocks(s.header.kernRightEntryCount) + 1)
                             .release();
      if (!s.kernBlockIndex) LOG_DBG("SDCF", "No kern index for style %u; streaming whole tables", i);
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    // A bad shaping section only costs complex-script shaping, not the font.
    // Page caches store the layout font's glyph IDs, so its hash joins the font's.
    if (shapingOffsets[i] != 0 && loadShapingSection(file, i, shapingOffsets[i])) {
      contentHash_ =
          fnv1a::hash(reinterpret_cast<const uint8_t*>(&s.shapingBlobKey), sizeof(s.shapingBlobKey), contentHash_);
    }
    applyGlyphMissCallback(i);
  }

  file.close();
  if (leanContent_) {
    for (auto& s : styles_) {
      if (s.present && !loadStyleLigatures(s)) {
        freeAll();
        return false;
      }
    }
  }
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto iv = s.intervalAt(static_cast<uint32_t>(mid));
    const uint32_t first = iv.first;
    const uint32_t last = iv.last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(iv.offset + (codepoint - first));
    }
  }
  return -1;
}

bool SdCardFont::readAdvance(uint32_t codepoint, uint8_t style, uint16_t* outAdvance) const {
  if (!outAdvance || !loaded_) return false;

  const uint8_t styleIdx = resolveStyle(style);
  if (styleIdx >= MAX_STYLES || !styles_[styleIdx].present) return false;
  const auto& s = styles_[styleIdx];
  if (advanceTableLookup(styleIdx, codepoint, outAdvance)) return true;
  if (!s.fullIntervals && !s.bmpIntervals) return false;

  int32_t glyphIndex = findGlobalGlyphIndex(s, codepoint);
  if (glyphIndex < 0 && codepoint != REPLACEMENT_GLYPH) {
    glyphIndex = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
  }
  if (glyphIndex < 0) return false;

  if (contentMetrics_) {
    const auto* glyph = contentGlyphMetrics(codepoint, styleIdx, glyphIndex);
    if (!glyph) return false;
    *outAdvance = glyph->glyph.advanceX;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "readAdvance: failed to open .cpfont for U+%04X style %u", codepoint, styleIdx);
    return false;
  }

  const uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(glyphIndex) * sizeof(EpdGlyph);
  EpdGlyph glyph = {};
  if (!file.seekSet(fileOff) || file.read(reinterpret_cast<uint8_t*>(&glyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "readAdvance: failed to read glyph for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return false;
  }
  file.close();

  *outAdvance = glyphEntryValid(s, glyph, codepoint) ? glyph.advanceX : 0;
  return true;
}

SdCardFont::ContentMetricsEntry* SdCardFont::contentGlyphMetrics(const uint32_t codepoint, const uint8_t styleIdx,
                                                                 const int32_t glyphIndex) const {
  // Probe the fixed table without moving entries: EpdGlyph pointers identify
  // their bitmap owners. A hash collision alone must not discard cached pixels.
  const uint32_t hash = (codepoint ^ (uint32_t(styleIdx) << 21)) * 2654435761U;
  const uint32_t first = hash >> 25;
  auto* victim = &contentMetrics_[first];
  for (uint32_t probe = 0; probe < CONTENT_METRICS_CACHE_SIZE; ++probe) {
    auto& candidate = contentMetrics_[(first + probe) % CONTENT_METRICS_CACHE_SIZE];
    if (candidate.codepoint == codepoint && candidate.style == styleIdx) {
      if (!leanContent_) candidate.recentlyUsed = 1;
      return &candidate;
    }
    if (!leanContent_ && candidate.style == MAX_STYLES) {
      // Body entries are never deleted individually, so no key can sit
      // beyond the first empty slot until the table has filled.
      victim = &candidate;
      break;
    }
    // Search all slots even after finding a vacancy: ring eviction and metrics
    // replacement leave holes before live keys. Prefer unused, then bitmapless
    // entries; only a table entirely backed by bitmaps requires their eviction.
    if ((candidate.style == MAX_STYLES && victim->style != MAX_STYLES) ||
        (!candidate.bitmapValid && victim->bitmapValid))
      victim = &candidate;
  }
  if (!leanContent_ && victim->style != MAX_STYLES) {
    // The body cache holds no bitmaps. Once full, give active glyphs another
    // turn instead of evicting the same frequently used hash slot on each miss.
    while (contentMetrics_[bodyMetricsClock_].recentlyUsed) {
      contentMetrics_[bodyMetricsClock_].recentlyUsed = 0;
      bodyMetricsClock_ = (bodyMetricsClock_ + 1) % CONTENT_METRICS_CACHE_SIZE;
    }
    victim = &contentMetrics_[bodyMetricsClock_];
    bodyMetricsClock_ = (bodyMetricsClock_ + 1) % CONTENT_METRICS_CACHE_SIZE;
  }
  auto& entry = *victim;
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Cannot open content font metrics");
    return nullptr;
  }
  EpdGlyph glyph{};
  const uint32_t offset = styles_[styleIdx].glyphsFileOffset + uint32_t(glyphIndex) * sizeof(EpdGlyph);
  const bool ok = file.seekSet(offset) && file.read(&glyph, sizeof(glyph)) == sizeof(glyph);
  file.close();
  if (!ok) {
    LOG_ERR("SDCF", "Cannot read content glyph metrics for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }
  if (!glyphEntryValid(styles_[styleIdx], glyph, codepoint)) glyph = EpdGlyph{};
  entry = {codepoint, glyph, styleIdx};
  if (!leanContent_) entry.recentlyUsed = 1;
  return &entry;
}

void SdCardFont::prepareContentBitmaps() {
  if (contentBitmapsAttempted_) return;
  contentBitmapsAttempted_ = true;
  if (!contentBitmapBytes_) return;
  const size_t idealBytes = size_t(std::min(contentBitmapBytes_, CONTENT_BITMAP_SLOT_MAX)) * CONTENT_METRICS_CACHE_SIZE;
  size_t bytes = idealBytes;
  if (psramHeapAvailable()) {
    if (!MemoryBudget::canAllocatePsram(bytes)) return;
    contentBitmaps_ = makePsramByteBufferNoThrow(bytes);
  } else {
    bytes = std::min<size_t>(idealBytes, contentBitmapInternalLimit_);
    if (!bytes || !MemoryBudget::canAllocateInternal(bytes, MemoryBudget::EPUB_FONT_INTERNAL_RESERVE,
                                                     MINI_RETAIN_MIN_MAX_ALLOC_HEAP + bytes))
      return;
    contentBitmaps_ = makeInternalByteBufferNoThrow(bytes);
  }
  // One font-lifetime buffer: up to 8KB inside the C3 resident ceiling, or
  // 64KB of PSRAM. Too large for stack; reused across pages and gray passes.
  if (!contentBitmaps_) {
    LOG_ERR("SDCF", "Cannot allocate content bitmap cache (%zu bytes); streaming glyphs", bytes);
    return;
  }
  contentBitmapCapacity_ = bytes;
  LOG_DBG("SDCF", "Content bitmap cache: %zu bytes in %s", bytes,
          memoryPoolName(byteBufferPool(contentBitmaps_.get())));
}

int SdCardFont::contentMetricsIndex(const EpdGlyph* glyph) const {
  if (!contentMetrics_ || !glyph) return -1;
  // Compare integer addresses before deriving an index: subtracting unrelated
  // C++ pointers would be undefined for ordinary overflow/built-in glyphs.
  const uintptr_t offset = reinterpret_cast<uintptr_t>(glyph) - reinterpret_cast<uintptr_t>(&contentMetrics_[0].glyph);
  return offset < CONTENT_METRICS_CACHE_BYTES && offset % sizeof(ContentMetricsEntry) == 0
             ? static_cast<int>(offset / sizeof(ContentMetricsEntry))
             : -1;
}

// Adapted from CrossPoint #4091: reject damaged metadata before any bitmap read.
bool SdCardFont::glyphEntryValid(const PerStyle& s, const EpdGlyph& glyph, const uint32_t codepoint) const {
  const uint32_t pixels = static_cast<uint32_t>(glyph.width) * glyph.height;
  const uint32_t packed = s.header.is2Bit ? (pixels + 3) / 4 : (pixels + 7) / 8;
  const uint64_t end = static_cast<uint64_t>(s.bitmapFileOffset) + glyph.dataOffset + glyph.dataLength;
  if (glyph.dataLength == packed && end <= fileSize_) return true;
  if (corruptGlyphsLogged_ == 0) corruptGlyphNotice_ = true;
  if (corruptGlyphsLogged_ < 3) {
    ++corruptGlyphsLogged_;
    LOG_ERR("SDCF", "Corrupt glyph U+%04X in %s; drawing it as missing", static_cast<unsigned>(codepoint), filePath_);
  }
  return false;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, const bool includeKerning) {
  lastPrewarmFailed_ = false;
  if (!loaded_) return failPrewarm(-1);
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;
  if (leanContent_) {
    for (uint8_t style = 0; style < MAX_STYLES; ++style) {
      if ((styleMask & (1 << style)) && !loadStyleLigatures(styles_[style])) return failPrewarm(-1);
    }
    if (!metadataOnly) prepareContentBitmaps();
    return 0;  // Cache small glyphs when admitted; otherwise reuse the fixed scratch.
  }

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return failPrewarm(-1);
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (shaping::isPositionToken(cp) || utf8IsVariationSelector(cp)) continue;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // ligatures for all styles upfront. Full rendering streams the kern classes
  // separately in prewarmStyle().
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleLigatures(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints.get(), cpCount, metadataOnly, includeKerning);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::failPrewarm(const int missed) {
  lastPrewarmFailed_ = true;
  return missed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                             const bool includeKerning) {
  auto& s = styles_[styleIdx];

  // Idle-prewarm hit: mini data persists across PrewarmScopes (resetStyleMiniData
  // keeps it), so when the previous scope -- typically the idle prewarm of this
  // exact page -- already loaded every requested codepoint the font covers, this
  // page needs zero SD reads. A mini built metadata-only cannot serve a full
  // request (no bitmaps). Any uncovered codepoint falls through to the rebuild.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
        if (cp < s.miniIntervals[iv].first) break;  // intervals sorted ascending
        if (cp <= s.miniIntervals[iv].last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;  // not in font coverage: the rebuild couldn't load it either
      } else {
        covered = false;
      }
    }
    if (covered) {
      if (!metadataOnly) {
        loadStyleLigatures(s);
        if (includeKerning && !s.miniKernBuilt) {
          // At most MAX_PAGE_GLYPHS * 4 bytes, transient: keep this off the task stack.
          auto resident = makeUniqueNoThrow<uint32_t[]>(s.miniGlyphCount);
          uint32_t count = 0;
          if (resident) {
            for (uint32_t iv = 0; iv < s.miniIntervalCount; ++iv) {
              for (uint32_t cp = s.miniIntervals[iv].first; cp <= s.miniIntervals[iv].last && count < s.miniGlyphCount;
                   ++cp) {
                resident[count++] = cp;
              }
            }
          } else {
            LOG_ERR("SDCF", "Failed to allocate resident kern codepoints (%u)", s.miniGlyphCount);
          }
          const bool ok = buildMiniKernMatrix(s, resident ? resident.get() : codepoints, resident ? count : cpCount);
          s.miniKernBuilt = ok && resident != nullptr;
        }
        // Dictionary requests hide kerning but retain the cached matrix for the reader.
        // A failed kern build still publishes successfully loaded ligatures.
        applyKernLigaturePointers(s, s.miniData, includeKerning);
      }
      return missedInMini;
    }
  }

  // Preserve the glyphs already resident in the mini arena when rebuilding.
  // A page body and its CJK status-bar title are warmed separately; replacing
  // the arena made each pass evict the other and repeat SD reads forever.
  // Bound the union to MAX_PAGE_GLYPHS so the retained render cache remains
  // within the same memory ceiling as a single dense page.
  std::unique_ptr<uint32_t[]> unionCps;
  bool incremental = false;
  if (s.miniGlyphCount > 0 && s.miniIntervalCount > 0 && ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP) {
    const uint32_t unionMaxCount = s.miniGlyphCount + cpCount;
    const uint32_t avgBitmapBytes =
        (s.miniBitmapUsed > 0 && s.miniGlyphCount > 0) ? s.miniBitmapUsed / s.miniGlyphCount : 64;
    const uint32_t estimatedArenaBytes = unionMaxCount * (static_cast<uint32_t>(sizeof(EpdGlyph)) + avgBitmapBytes);
    constexpr uint32_t UNION_PRESSURE_HEADROOM = 12U * 1024U;
    if (estimatedArenaBytes + UNION_PRESSURE_HEADROOM > ESP.getFreeHeap()) {
      freeStyleMiniData(s);
    }
  }
  if (s.miniGlyphCount > 0 && s.miniIntervalCount > 0) {
    const uint32_t unionMax = s.miniGlyphCount + cpCount;
    unionCps = makeUniqueNoThrow<uint32_t[]>(unionMax);
    if (unionCps) {
      uint32_t count = 0;
      uint32_t intervalIndex = 0;
      uint32_t intervalCodepoint = s.miniIntervals[0].first;
      bool intervalActive = true;
      uint32_t requestIndex = 0;
      while ((intervalActive || requestIndex < cpCount) && count < unionMax) {
        uint32_t next;
        if (intervalActive && (requestIndex >= cpCount || intervalCodepoint <= codepoints[requestIndex])) {
          next = intervalCodepoint;
          if (requestIndex < cpCount && codepoints[requestIndex] == intervalCodepoint) requestIndex++;
          if (intervalCodepoint < s.miniIntervals[intervalIndex].last) {
            intervalCodepoint++;
          } else if (++intervalIndex < s.miniIntervalCount) {
            intervalCodepoint = s.miniIntervals[intervalIndex].first;
          } else {
            intervalActive = false;
          }
        } else {
          next = codepoints[requestIndex++];
        }
        unionCps[count++] = next;
      }
      if (!intervalActive && requestIndex >= cpCount && count <= MAX_PAGE_GLYPHS) {
        // A full resident mini cannot be replaced with metadata-only entries.
        metadataOnly = metadataOnly && s.miniMetadataOnly;
        codepoints = unionCps.get();
        cpCount = count;
        // Metadata-only caches keep file offsets, so promoting one to rendered
        // bitmaps still uses the cold rebuild path.
        incremental = metadataOnly || !s.miniMetadataOnly;
      }
    } else {
      LOG_ERR("SDCF", "Failed to allocate resident glyph union (%u)", unionMax);
      return failPrewarm(static_cast<int>(cpCount));
    }
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  auto mappings = makeUniqueNoThrow<CpGlyphMapping[]>(cpCount);
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return failPrewarm(static_cast<int>(cpCount));
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Stage at most 512 glyphs + intervals (14 KiB) off-stack. Keeping the old
  // metadata until commit makes read/OOM failures harmless to resident glyphs.
  // Under pressure retain the existing cold, free-before-grow rebuild policy.
  const uint32_t stagedBytes = validCount * (sizeof(EpdGlyph) + sizeof(EpdUnicodeInterval));
  if (incremental && (ESP.getFreeHeap() < stagedBytes + MINI_RETAIN_MIN_FREE_HEAP ||
                      ESP.getMaxAllocHeap() < MINI_RETAIN_MIN_MAX_ALLOC_HEAP)) {
    incremental = false;
  }
  std::unique_ptr<EpdUnicodeInterval[]> stagedIntervals;
  std::unique_ptr<EpdGlyph[]> stagedGlyphs;
  if (incremental) {
    stagedIntervals = makeUniqueNoThrow<EpdUnicodeInterval[]>(validCount);
    stagedGlyphs = makeUniqueNoThrow<EpdGlyph[]>(validCount);
    if (!stagedIntervals || !stagedGlyphs) {
      LOG_ERR("SDCF", "Failed to stage incremental font metadata (%u glyphs)", validCount);
      return failPrewarm(static_cast<int>(cpCount));
    }
  } else {
    s.miniIntervalCount = 0;
    s.miniGlyphCount = 0;
    s.miniBitmapUsed = 0;
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
    memset(&s.miniData, 0, sizeof(s.miniData));
    s.epdFont.data = &s.stubData;
    if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, validCount) ||
        !ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, validCount)) {
      LOG_ERR("SDCF", "Failed to allocate mini metadata for style %u", styleIdx);
      freeStyleMiniData(s);
      return failPrewarm(static_cast<int>(cpCount));
    }
  }
  auto* intervals = incremental ? stagedIntervals.get() : s.miniIntervals;
  auto* glyphs = incremental ? stagedGlyphs.get() : s.miniGlyphs;
  uint32_t intervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      intervals[intervalCount++] = {mappings[rangeStart].codepoint, mappings[i - 1].codepoint, rangeStart};
      rangeStart = i;
    }
  }

  // Read only missing entries; resident glyphs already have mini-bitmap offsets.
  auto readOrder = makeUniqueNoThrow<uint32_t[]>(validCount);
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    if (!incremental) freeStyleMiniData(s);
    return failPrewarm(static_cast<int>(cpCount));
  }
  uint32_t readCount = 0;
  uint32_t residentInterval = 0;
  for (uint32_t i = 0; i < validCount; ++i) {
    const uint32_t cp = mappings[i].codepoint;
    if (incremental) {
      while (residentInterval < s.miniIntervalCount && s.miniIntervals[residentInterval].last < cp) {
        ++residentInterval;
      }
      if (residentInterval < s.miniIntervalCount && s.miniIntervals[residentInterval].first <= cp) {
        const auto& iv = s.miniIntervals[residentInterval];
        glyphs[i] = s.miniGlyphs[iv.offset + cp - iv.first];
        continue;
      }
    }
    readOrder[readCount++] = i;
  }
  std::sort(readOrder.get(), readOrder.get() + readCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  HalFile file;
  const auto failed = [&]() {
    file.close();
    if (!incremental) freeStyleMiniData(s);
    return failPrewarm(static_cast<int>(cpCount));
  };
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    return failed();
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < readCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        return failed();
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&glyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      return failed();
    }
    if (!glyphEntryValid(s, glyphs[mapIdx], mappings[mapIdx].codepoint)) glyphs[mapIdx] = EpdGlyph{};
    lastReadIndex = gIdx;
  }

  // Mapping is no longer needed once glyph metadata has been read. Releasing
  // it before the bitmap request leaves a larger contiguous heap region.
  mappings.reset();
  uint32_t loadedBitmapBytes = 0;
  uint32_t totalBitmapSize = incremental ? s.miniBitmapUsed : 0;
  HeapByteBuffer stagedBitmap;

  if (!metadataOnly) {
    for (uint32_t i = 0; i < readCount; i++) {
      loadedBitmapBytes += glyphs[readOrder[i]].dataLength;
    }
    totalBitmapSize += loadedBitmapBytes;
    if (incremental) {
      if (totalBitmapSize > s.miniBitmapCapacity) {
        // A temporary replacement retains the old pixels until I/O succeeds.
        // The live old buffer is included in the allocator's free-space snapshot;
        // require the font reserve before admitting this extra payload.
        stagedBitmap = allocateMiniBitmap(totalBitmapSize, true);
        if (!stagedBitmap) {
          LOG_ERR("SDCF", "Failed to stage mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
          return failed();
        }
        memcpy(stagedBitmap.get(), s.miniBitmap.get(), s.miniBitmapUsed);
      }
    } else {
      // Preserve cold rebuild's smaller peak: this list is reconstructible once
      // all metadata is loaded, so release it before a growing bitmap allocation.
      if (totalBitmapSize > s.miniBitmapCapacity) readOrder.reset();
      if (!ensureBitmapCapacity(s, totalBitmapSize)) {
        LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
        return failed();
      }
      if (!readOrder) {
        readOrder = makeUniqueNoThrow<uint32_t[]>(readCount);
        if (!readOrder) {
          LOG_ERR("SDCF", "Failed to allocate bitmap read order for style %u", styleIdx);
          return failed();
        }
        for (uint32_t i = 0; i < readCount; ++i) readOrder[i] = i;
      }
    }
    uint8_t* bitmap = stagedBitmap ? stagedBitmap.get() : s.miniBitmap.get();

    // Read bitmap data sorted by file offset
    std::sort(readOrder.get(), readOrder.get() + readCount,
              [&](uint32_t a, uint32_t b) { return glyphs[a].dataOffset < glyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = incremental ? s.miniBitmapUsed : 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < readCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = glyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          return failed();
        }
        seekCount++;
      }
      if (file.read(bitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        return failed();
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  readOrder.reset();
  file.close();

  if (incremental) {
    s.epdFont.data = &s.stubData;
    delete[] s.miniIntervals;
    delete[] s.miniGlyphs;
    // PerStyle owns these arrays and frees them in freeStyleMiniData().
    s.miniIntervals = stagedIntervals.release();
    s.miniGlyphs = stagedGlyphs.release();
    s.miniIntervalCapacity = s.miniGlyphCapacity = validCount;
    if (stagedBitmap) {
      s.miniBitmap = std::move(stagedBitmap);
      s.miniBitmapCapacity = totalBitmapSize;
      s.miniBitmapPool = byteBufferPool(s.miniBitmap.get());
    }
  }
  s.miniIntervalCount = intervalCount;
  s.miniGlyphCount = validCount;
  s.miniBitmapUsed = metadataOnly ? 0 : totalBitmapSize;

  // Ligatures stay available to dictionary text even when kerning is disabled.
  // Kern failures must not discard successful ligatures.
  bool ligaturesOk = false;
  s.miniKernBuilt = false;
  if (!metadataOnly) {
    ligaturesOk = loadStyleLigatures(s);
    if (includeKerning) s.miniKernBuilt = buildMiniKernMatrix(s, codepoints, cpCount);
  }

  // Populate miniData and swap
  s.miniMetadataOnly = metadataOnly;
  s.miniHysteresisPending = !metadataOnly;  // one hysteresis evaluation per rebuild
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = metadataOnly ? nullptr : s.miniBitmap.get();
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (ligaturesOk || s.miniKernBuilt) {
    applyKernLigaturePointers(s, s.miniData, includeKerning);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.coverageHandler = &SdCardFont::onCoverageQuery;
  s.miniData.shapeHandler = s.shaper ? &SdCardFont::onShape : nullptr;

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += readCount;
  stats_.bitmapBytes += loadedBitmapBytes;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::releaseForLowMemory(const bool preserveAdvanceTable) {
  clearOverflow();
  contentBitmaps_.reset();
  contentBitmapCapacity_ = 0;
  contentBitmapNext_ = 0;
  contentBitmapsAttempted_ = false;
  if (!leanContent_) {
    contentMetrics_.reset();
    bodyMetricsAttempted_ = false;
    bodyMetricsClock_ = 0;
  } else if (contentMetrics_) {
    for (size_t i = 0; i < CONTENT_METRICS_CACHE_SIZE; ++i) contentMetrics_[i].bitmapValid = false;
  }
  if (!preserveAdvanceTable) {
    clearPersistentCache();
  }

  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    // Optional-size ligatures belong to its budgeted resident data. Layout reads
    // them without prewarming; dropping them here would change widths after a redraw.
    if (!leanContent_) freeStyleLigatures(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  if (contentMetrics_) std::fill_n(contentMetrics_.get(), CONTENT_METRICS_CACHE_SIZE, ContentMetricsEntry{});
  contentBitmapNext_ = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
    advanceTableCapacity_[i] = 0;
  }
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

bool SdCardFont::ensureAdvanceTableCapacity(const uint8_t styleIdx, const uint32_t needed) {
  if (advanceTableCapacity_[styleIdx] >= needed) return true;

  uint32_t newCapacity = advanceTableCapacity_[styleIdx] == 0 ? 1 : advanceTableCapacity_[styleIdx];
  while (newCapacity < needed && newCapacity < ADVANCE_CACHE_LIMIT) {
    newCapacity <<= 1;
  }
  if (newCapacity > ADVANCE_CACHE_LIMIT) newCapacity = ADVANCE_CACHE_LIMIT;

  auto* replacement = makeUniqueNoThrow<AdvanceEntry[]>(newCapacity).release();
  if (!replacement) {
    LOG_ERR("SDCF", "Advance table grow failed (%u bytes, style %u, free=%u, maxAlloc=%u)",
            static_cast<unsigned>(newCapacity * sizeof(AdvanceEntry)), styleIdx, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  if (advanceTable_[styleIdx] && advanceTableSize_[styleIdx] > 0) {
    memcpy(replacement, advanceTable_[styleIdx], advanceTableSize_[styleIdx] * sizeof(AdvanceEntry));
  }

  delete[] advanceTable_[styleIdx];
  advanceTable_[styleIdx] = replacement;
  advanceTableCapacity_[styleIdx] = newCapacity;
  return true;
}

void SdCardFont::mergeIntoAdvanceTable(const uint8_t styleIdx, const AdvanceEntry* sortedNew, const uint32_t newCount) {
  if (newCount == 0) return;
  const uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) return;  // already full

  // Cap the merged size at ADVANCE_CACHE_LIMIT. Anything past the cap is
  // dropped from the tail of the sorted merge — a deterministic, bounded loss
  // that doesn't bias which codepoints get cached on subsequent passes.
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  if (!ensureAdvanceTableCapacity(styleIdx, mergedCap)) return;
  // serialx, CrossPoint #3831: merge backwards in spare capacity, dropping
  // the largest entries past our existing 256-entry cap before writing.
  AdvanceEntry* table = advanceTable_[styleIdx];
  uint32_t drop = oldSize + newCount - mergedCap;
  uint32_t i = oldSize, j = newCount, k = oldSize + newCount;
  while (j > 0) {
    const bool takeNew = i == 0 || sortedNew[j - 1].codepoint >= table[i - 1].codepoint;
    const AdvanceEntry entry = takeNew ? sortedNew[--j] : table[--i];
    --k;
    if (drop > 0)
      --drop;
    else
      table[k] = entry;
  }
  advanceTableSize_[styleIdx] = mergedCap;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTableSize_[i] > 0) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  if (!advanceTable_[style]) return 0;
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  return 0;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const auto& s = styles_[si];

    if (advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT) {
      bool cacheMissesRequestedCodepoint = false;
      for (uint32_t i = 0; i < cpCount; i++) {
        if (!advanceTableLookup(si, codepoints[i], nullptr)) {
          cacheMissesRequestedCodepoint = true;
          break;
        }
      }
      if (!cacheMissesRequestedCodepoint) continue;

      advanceTableSize_[si] = 0;
      LOG_DBG("SDCF", "Advance table style %u: reset full cache for active text (capacity=%u)", si,
              advanceTableCapacity_[si]);
    }

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (advanceTableLookup(si, cp, nullptr)) continue;  // already cached
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Open file once and read advanceX for each needed glyph.
    HalFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }

    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      file.close();
      continue;
    }

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
          break;
        }
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = glyphEntryValid(s, tempGlyph, mappings[i].codepoint) ? tempGlyph.advanceX : 0;
      fetched++;
    }
    file.close();

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask,
                                       const char* extraText) {
  if (!loaded_) return -1;
  if (leanContent_) return 0;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Grow before transient layout scratch so the persistent table does not occupy its freed hole.
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const uint32_t size = advanceTableSize_[si];
    if (size < ADVANCE_CACHE_LIMIT && advanceTableCapacity_[si] - size < 64) {
      ensureAdvanceTableCapacity(si, std::min(size + 64, ADVANCE_CACHE_LIMIT));
    }
  }

  // Preserve CrossInk's active-text refresh: a full cache is reusable only
  // when every requested character is already cached. This prepass allocates nothing.
  const auto visit = [&](auto accept, auto reserved) {
    const auto text = [&](const char* value) {
      const auto* p = reinterpret_cast<const unsigned char*>(value);
      while (*p) {
        const uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        if (!utf8IsVariationSelector(cp)) accept(cp);
      }
    };
    for (auto it = begin; it != end; ++it) text(asCStr(*it));
    if (extraText) text(extraText);
    if (includeSpace) reserved(' ');
    if (includeHyphen) reserved('-');
  };
  uint8_t openStyles = 0;
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    bool needsFetch = advanceTableSize_[si] < ADVANCE_CACHE_LIMIT;
    if (!needsFetch) {
      const auto check = [&](uint32_t cp) {
        if (!advanceTableLookup(si, cp, nullptr)) needsFetch = true;
      };
      visit(check, check);
    }
    if (needsFetch) openStyles |= 1 << si;
  }
  if (openStyles == 0) {
    stats_.prewarmTotalMs = millis() - startMs;
    return 0;
  }

  uint32_t limit = 0;
  for (auto it = begin; it != end && limit < ADVANCE_CACHE_LIMIT; ++it)
    limit += countUtf8Codepoints(asCStr(*it), ADVANCE_CACHE_LIMIT - limit);
  if (extraText && limit < ADVANCE_CACHE_LIMIT) limit += countUtf8Codepoints(extraText, ADVANCE_CACHE_LIMIT - limit);
  // 136 bytes on stack for short text; at most 1032 heap bytes for a paragraph.
  // A chapter-sized stack buffer is unsafe on the reader task's small stack.
  constexpr uint32_t INLINE_CODEPOINTS = 32;
  uint32_t inlineCodepoints[INLINE_CODEPOINTS + 2];
  std::unique_ptr<uint32_t[]> heapCodepoints;
  uint32_t* codepoints = inlineCodepoints;
  if (limit > INLINE_CODEPOINTS) {
    heapCodepoints = makeUniqueNoThrow<uint32_t[]>(limit + 2);
    if (!heapCodepoints) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes)",
              static_cast<unsigned>((limit + 2) * sizeof(uint32_t)));
      return -1;
    }
    codepoints = heapCodepoints.get();
  }
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    if (!(openStyles & (1 << si))) continue;
    const auto& style = styles_[si];
    const bool refresh = advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT;
    const bool replacement = findGlobalGlyphIndex(style, REPLACEMENT_GLYPH) >= 0;
    const auto resolves = [&](uint32_t cp) { return replacement || findGlobalGlyphIndex(style, cp) >= 0; };
    bool anyLacking = false;
    const auto wanted = [&](uint32_t cp) {
      if (!refresh && advanceTableLookup(si, cp, nullptr)) return false;
      if (resolves(cp)) return true;
      anyLacking = true;
      return false;
    };
    LowestCodepoints collected(codepoints, limit);
    visit(
        [&](uint32_t cp) {
          if (wanted(cp)) collected.add(cp);
        },
        [&](uint32_t cp) {
          if (wanted(cp)) collected.addReserved(cp);
        });
    std::sort(codepoints, codepoints + collected.size());
    // A full table must refresh even if the bounded lowest subset happens to
    // be cached: the prepass inspected the entire request, not just that subset.
    if (refresh) advanceTableSize_[si] = 0;
    totalMissed += fetchAdvancesForCodepoints(codepoints, collected.size(), 1 << si);
    if (anyLacking) {
      LowestCodepoints lacking(codepoints, limit);
      visit(
          [&](uint32_t cp) {
            if (!resolves(cp)) lacking.add(cp);
          },
          [&](uint32_t cp) {
            if (!resolves(cp)) lacking.addReserved(cp);
          });
      totalMissed += static_cast<int>(lacking.size());
    }
  }
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask, const char* extraText) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, false, false, styleMask, extraText);
}

int SdCardFont::buildAdvanceTable(const std::deque<std::string>& words, bool includeHyphen, uint8_t styleMask,
                                  const char* extraText) {
  return buildAdvanceTableRange(words.begin(), words.end(), words.size() > 1, includeHyphen, styleMask, extraText);
}

int SdCardFont::buildAdvanceTableForCodepoints(const uint32_t* sourceCodepoints, uint32_t cpCount, bool includeSpace,
                                               bool includeHyphen, uint8_t styleMask) {
  if (!loaded_) return -1;
  if (leanContent_) return 0;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  const unsigned long startMs = millis();
  // Grow before transient layout scratch so the persistent table does not occupy its freed hole.
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const uint32_t size = advanceTableSize_[si];
    if (size < ADVANCE_CACHE_LIMIT && advanceTableCapacity_[si] - size < 64) {
      ensureAdvanceTableCapacity(si, std::min(size + 64, ADVANCE_CACHE_LIMIT));
    }
  }

  const uint32_t extraCount = (includeSpace ? 1U : 0U) + (includeHyphen ? 1U : 0U);
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[cpCount + extraCount]);
  if (!codepoints) {
    LOG_ERR("SDCF", "buildAdvanceTableForCodepoints: failed to allocate codepoint buffer (%u bytes)",
            static_cast<unsigned>((cpCount + extraCount) * sizeof(uint32_t)));
    return -1;
  }

  uint32_t outCount = 0;
  for (uint32_t i = 0; i < cpCount; ++i) {
    const uint32_t cp = sourceCodepoints[i];
    if (cp == 0 || utf8IsVariationSelector(cp)) continue;
    codepoints[outCount++] = cp;
  }
  if (includeSpace) codepoints[outCount++] = ' ';
  if (includeHyphen) codepoints[outCount++] = '-';

  std::sort(codepoints.get(), codepoints.get() + outCount);
  outCount = static_cast<uint32_t>(std::unique(codepoints.get(), codepoints.get() + outCount) - codepoints.get());
  const int totalMissed = fetchAdvancesForCodepoints(codepoints.get(), outCount, styleMask);
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Ruled layout measures body glyphs before page bitmaps are prepared. Reuse
  // the optional-size metrics cache so repeated letters need no SD pixel reads.
  if (self->metricsDepth_ && !self->leanContent_ && !self->bodyMetricsAttempted_) {
    self->bodyMetricsAttempted_ = true;
    if (MemoryBudget::canAllocateInternal(CONTENT_METRICS_CACHE_BYTES, MemoryBudget::EPUB_LAYOUT_INTERNAL_RESERVE,
                                          MINI_RETAIN_MIN_MAX_ALLOC_HEAP + CONTENT_METRICS_CACHE_BYTES)) {
      self->contentMetrics_ = makeUniqueNoThrow<ContentMetricsEntry[]>(CONTENT_METRICS_CACHE_SIZE);
      if (!self->contentMetrics_) LOG_ERR("SDCF", "Body metrics cache unavailable; using overflow glyphs");
    }
  }
  if (self->metricsDepth_ && self->contentMetrics_) {
    const int32_t index = self->findGlobalGlyphIndex(s, codepoint);
    const auto* entry = index < 0 ? nullptr : self->contentGlyphMetrics(codepoint, styleIdx, index);
    return entry ? &entry->glyph : nullptr;
  }
  if (self->contentBitmaps_) {
    const int32_t index = self->findGlobalGlyphIndex(s, codepoint);
    auto* entry = index < 0 ? nullptr : self->contentGlyphMetrics(codepoint, styleIdx, index);
    if (!entry) return nullptr;
    const auto& glyph = entry->glyph;
    if (glyph.dataLength <= CONTENT_BITMAP_SLOT_MAX && glyph.dataLength <= self->contentBitmapCapacity_) {
      if (!entry->bitmapValid) {
        // Wrap as a whole glyph, then invalidate every overlapping resident
        // before reading: even a short/failed read can overwrite old pixels.
        if (self->contentBitmapNext_ + glyph.dataLength > self->contentBitmapCapacity_) self->contentBitmapNext_ = 0;
        const uint32_t start = self->contentBitmapNext_;
        const uint32_t end = start + glyph.dataLength;
        for (size_t i = 0; i < CONTENT_METRICS_CACHE_SIZE; ++i) {
          auto& resident = self->contentMetrics_[i];
          if (resident.bitmapValid && resident.bitmapOffset < end &&
              uint32_t(resident.bitmapOffset) + resident.glyph.dataLength > start)
            resident.bitmapValid = false;
        }
        HalFile file;
        if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
          LOG_ERR("SDCF", "Cannot open content bitmap");
          return nullptr;
        }
        const bool ok = file.seekSet(s.bitmapFileOffset + glyph.dataOffset) &&
                        file.read(self->contentBitmaps_.get() + start, glyph.dataLength) == glyph.dataLength;
        file.close();
        if (!ok) {
          LOG_ERR("SDCF", "Cannot read content bitmap for U+%04X style %u", codepoint, styleIdx);
          return nullptr;
        }
        entry->bitmapOffset = start;
        entry->bitmapValid = true;
        self->contentBitmapNext_ = end;
      }
      return &entry->glyph;
    }
  }

  // The lean path shares one bitmap: only the most recently read slot still
  // owns valid pixels. Repeated metric/draw queries for that glyph reuse it.
  auto* scratchOwner = self->contentScratchOwner_ ? self->contentScratchOwner_ : self;
  const uint32_t latest = (self->overflowNext_ + OVERFLOW_CAPACITY - 1) % OVERFLOW_CAPACITY;
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->leanContent_ && (i != latest || scratchOwner->contentScratchUser_ != self)) continue;
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // A failed read may overwrite the shared scratch; do not reuse its old key.
  if (self->leanContent_) {
    if (self->overflowCount_) self->overflow_[latest].codepoint = 0;
    scratchOwner->contentScratchUser_ = nullptr;
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  if (!self->glyphEntryValid(s, tempGlyph, codepoint)) tempGlyph = EpdGlyph{};

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    if (self->leanContent_ && tempGlyph.dataLength > self->contentBitmapBytes_) {
      LOG_ERR("SDCF", "Content bitmap changed beyond its fixed scratch");
      file.close();
      return nullptr;
    }
    tempBitmap =
        self->leanContent_ ? scratchOwner->contentBitmap_.get() : new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      if (!self->leanContent_) delete[] tempBitmap;
      file.close();
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      if (!self->leanContent_) delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  file.close();
  if (wasAtCapacity) {
    if (!self->leanContent_) delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;
  if (self->leanContent_) scratchOwner->contentScratchUser_ = self;

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  if (contentMetricsIndex(glyph) >= 0) return true;
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  const int slot = contentMetricsIndex(glyph);
  if (slot >= 0) {
    return contentBitmaps_ && contentMetrics_[slot].bitmapValid
               ? contentBitmaps_.get() + contentMetrics_[slot].bitmapOffset
               : nullptr;
  }
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      if (leanContent_) {
        const auto* owner = contentScratchOwner_ ? contentScratchOwner_ : this;
        const uint32_t latest = (overflowNext_ + OVERFLOW_CAPACITY - 1) % OVERFLOW_CAPACITY;
        if (owner->contentScratchUser_ != this || i != latest) return nullptr;
      }
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
