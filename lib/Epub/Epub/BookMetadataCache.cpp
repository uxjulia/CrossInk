#include "BookMetadataCache.h"

#include <BufferedFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <cstring>
#include <limits>

#include "FsHelpers.h"

namespace {
constexpr uint32_t BOOK_CACHE_MAGIC = 0x425843FF;  // bytes: 0xFF, "CXB"
constexpr uint8_t BOOK_CACHE_VERSION = 10;         // v10: namespace-aware OPF parsing
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
constexpr size_t METADATA_ARENA_SLAB_BYTES = 4096;
// Reuse at most 32 KiB of arena-backed targets instead of one per chapter.
// This transient buffer is too large for the stack and is released after indexing.
constexpr size_t MAX_SIZE_LOOKUP_TARGETS = 2048;
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;
// Cap the reader-session speedup at 4KB on constrained C3 devices. EPUBs with
// larger spines keep using the existing on-disk lookup path.
constexpr uint16_t MAX_CACHED_CUMULATIVE_SIZES = 1024;

// A small cold-build optimization, not a reader-session cache. Keep the
// legacy cursor for tiny spines (the scan often completes sooner).
constexpr uint16_t MIN_EXACT_SIZE_BATCH_SPINE = 64;
constexpr size_t EXACT_SIZE_BATCH_TARGETS = 48;
constexpr size_t EXACT_SIZE_BATCH_PATH_BYTES = 2048;
constexpr size_t MAX_EXACT_SIZE_BATCH_PASSES = 8;

// Heap scratch (<4 KiB on C3), shared across batches, freed before serialization.
// Exact paths preserve matching semantics without extending the large-spine
// hashed lookup to books that previously used the exact ZIP cursor.
struct ExactSizeBatch {
  ZipFile::EntryTarget targets[EXACT_SIZE_BATCH_TARGETS];
  ZipFile::EntryIdentity identities[EXACT_SIZE_BATCH_TARGETS];
  char paths[EXACT_SIZE_BATCH_PATH_BYTES];
};

// Estimate the number of scans on the spine LUT pass that already reads every
// href. Normalizing an href only removes characters, so this is conservative.
struct ExactSizeBatchPlan {
  size_t passes = 1;
  size_t targets = 0;
  size_t bytes = 0;

  bool add(const std::string& rawHref) {
    const size_t need = rawHref.size() + 1;
    if (need > EXACT_SIZE_BATCH_PATH_BYTES || rawHref.find('\0') != std::string::npos) return false;
    if (targets == EXACT_SIZE_BATCH_TARGETS || need > EXACT_SIZE_BATCH_PATH_BYTES - bytes) {
      ++passes;
      targets = 0;
      bytes = 0;
    }
    ++targets;
    bytes += need;
    return passes <= MAX_EXACT_SIZE_BATCH_PASSES;
  }
};

// Entry (de)serializers, templated so they run over HalFile and the Buffered*
// wrappers alike (two instantiations each -- a few hundred bytes of flash, in
// exchange for the build path streaming at SD speed instead of per-pod).
template <typename F>
uint32_t writeSpineEntryTo(F& file, const BookMetadataCache::SpineEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

template <typename F>
uint32_t writeTocEntryTo(F& file, const BookMetadataCache::TocEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

template <typename F>
BookMetadataCache::SpineEntry readSpineEntryFrom(F& file) {
  BookMetadataCache::SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

template <typename F>
BookMetadataCache::TocEntry readTocEntryFrom(F& file) {
  BookMetadataCache::TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  lowMemoryFailure = false;
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  // Open spine file for writing
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  // Wrapper OOM is fine: createSpineEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(spineFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
  }
  return flushed;
}

bool BookMetadataCache::beginTocPass() {
  tocSpineMemo.clear();
  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.resetStorage();
    spineHrefIndexArena.release();
    if (!spineHrefIndexArena.init(METADATA_ARENA_SLAB_BYTES) || !spineHrefIndex.resize(spineCount)) {
      LOG_ERR("BMC", "Failed to allocate spine href index arena for %u spine items", spineCount);
      lowMemoryFailure = true;
      spineHrefIndex.resetStorage();
      spineHrefIndexArena.release();
      tocFile.close();
      spineFile.close();
      return false;
    }
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
  } else {
    useSpineHrefIndex = false;
  }

  // Wrapper OOM is fine: createTocEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(tocFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endTocPass() {
  tocSpineMemo.clear();
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing toc tmp file");
  }
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  spineHrefIndex.resetStorage();
  spineHrefIndexArena.release();
  useSpineHrefIndex = false;

  return flushed;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    return false;
  }
  auto closeBuildFiles = [this]() {
    bookFile.close();
    spineFile.close();
    tocFile.close();
  };

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_MAGIC) + sizeof(BOOK_CACHE_VERSION) +
                                   /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  // Buffered streams for the whole build: every access below is sequential per
  // file, but interleaved ACROSS files, which thrashes SdFat's single shared
  // sector cache when unbuffered (one 512B SD transaction per 4-byte pod --
  // measured 31s for a 1,732-spine omnibus). Three 4KB buffers, freed on return.
  serialization::BufferedFileWriter bookOut(bookFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader spineIn(spineFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader tocIn(tocFile, BUILD_IO_BUFFER_SIZE);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookOut, BOOK_CACHE_MAGIC);
  serialization::writePod(bookOut, BOOK_CACHE_VERSION);
  serialization::writePod(bookOut, lutOffset);
  serialization::writePod(bookOut, spineCount);
  serialization::writePod(bookOut, tocCount);
  // Metadata
  serialization::writeString(bookOut, metadata.title);
  serialization::writeString(bookOut, metadata.author);
  serialization::writeString(bookOut, metadata.language);
  serialization::writeString(bookOut, metadata.coverItemHref);
  serialization::writeString(bookOut, metadata.textReferenceHref);

  // Loop through spine entries, writing LUT positions and estimating batch work.
  ExactSizeBatchPlan sizeBatchPlan;
  bool tryExactSizeBatch = spineCount >= MIN_EXACT_SIZE_BATCH_SPINE && spineCount < LARGE_SPINE_THRESHOLD;
  spineIn.seek(0);
  for (int i = 0; i < spineCount; i++) {
    const uint32_t pos = spineIn.position();
    const auto entry = readSpineEntryFrom(spineIn);
    if (tryExactSizeBatch) tryExactSizeBatch = sizeBatchPlan.add(entry.href);
    serialization::writePod(bookOut, pos + lutOffset + lutSize);
  }
  // Total size of the spine tmp file: entries land in book.bin after the toc LUT
  // and the full spine block, so toc LUT positions are offset by it.
  const auto spineBytes = static_cast<uint32_t>(spineIn.position());

  // Loop through toc entries, writing LUT positions
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    const uint32_t pos = tocIn.position();
    readTocEntryFrom(tocIn);
    serialization::writePod(bookOut, pos + lutOffset + lutSize + spineBytes);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  Arena metadataArena;
  if (!metadataArena.init(METADATA_ARENA_SLAB_BYTES)) {
    LOG_ERR("BMC", "Failed to allocate metadata scratch arena (%u bytes)",
            static_cast<unsigned>(METADATA_ARENA_SLAB_BYTES));
    lowMemoryFailure = true;
    closeBuildFiles();
    return false;
  }

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  ArenaVector<int16_t> spineToTocIndex(metadataArena);
  if (!spineToTocIndex.resize(spineCount)) {
    LOG_ERR("BMC", "Failed to allocate spine-to-TOC index for %u spine items", spineCount);
    lowMemoryFailure = true;
    closeBuildFiles();
    return false;
  }
  for (size_t i = 0; i < spineToTocIndex.size(); ++i) {
    spineToTocIndex[i] = -1;
  }
  tocIn.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntryFrom(tocIn);
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    // Explicit close() required: member variables persist beyond function scope
    closeBuildFiles();
    return false;
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a bounded batch lookup that scans the ZIP
  // central directory once per chunk and matches spine targets by hash.
  // Extra scans for huge books trade I/O for bounded target scratch memory.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  ArenaVector<uint32_t> spineSizes(metadataArena);
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    ArenaVector<ZipFile::SizeTarget> targets(metadataArena);
    const size_t batchCapacity = std::min<size_t>(spineCount, MAX_SIZE_LOOKUP_TARGETS);
    if (!targets.resize(batchCapacity) || !spineSizes.resize(spineCount)) {
      LOG_ERR("BMC", "Failed to allocate batch size lookup scratch for %u spine items", spineCount);
      lowMemoryFailure = true;
      zip.close();
      closeBuildFiles();
      return false;
    }

    size_t targetCount = 0;
    int matched = 0;
    spineIn.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntryFrom(spineIn);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets[targetCount++] = t;
      if (targetCount < batchCapacity && i + 1 < spineCount) continue;

      std::sort(targets.begin(), targets.begin() + targetCount,
                [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
                  return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
                });
      matched += zip.fillUncompressedSizes(targets.data(), targetCount, spineSizes.data(), spineSizes.size());
      targetCount = 0;
    }
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    useBatchSizes = true;
  }

  if (tryExactSizeBatch) {
    constexpr size_t reserveBytes = 16 * 1024;
    constexpr size_t reserveContiguousBytes = 4 * 1024;
    const auto heap = byteHeapSnapshot(MemoryPool::Internal);
    auto batch = heap.free >= sizeof(ExactSizeBatch) + reserveBytes &&
                         heap.largest >= sizeof(ExactSizeBatch) + reserveContiguousBytes
                     ? makeUniqueNoThrow<ExactSizeBatch>()
                     : nullptr;
    if (batch && spineSizes.resize(spineCount)) {
      useBatchSizes = true;
      size_t targetCount = 0;
      size_t pathBytes = 0;
      size_t batchStart = 0;
      const auto flushBatch = [&]() {
        if (targetCount == 0) return true;
        std::fill_n(batch->identities, targetCount, ZipFile::EntryIdentity{});
        std::sort(batch->targets, batch->targets + targetCount,
                  [](const ZipFile::EntryTarget& a, const ZipFile::EntryTarget& b) {
                    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
                  });
        if (zip.fillEntryIdentities(batch->targets, targetCount, batch->identities, targetCount,
                                    /*exactScanEntryLimit=*/2u * spineCount) < 0) {
          return false;
        }
        for (size_t i = 0; i < targetCount; ++i) {
          spineSizes[batchStart + i] = batch->identities[i].uncompressedSize;
        }
        targetCount = 0;
        pathBytes = 0;
        return true;
      };

      spineIn.seek(0);
      for (size_t i = 0; i < spineCount; ++i) {
        auto entry = readSpineEntryFrom(spineIn);
        const std::string path = FsHelpers::normalisePath(entry.href);
        if (path.size() + 1 > EXACT_SIZE_BATCH_PATH_BYTES || path.find('\0') != std::string::npos) {
          useBatchSizes = false;
          break;
        }
        if ((targetCount == EXACT_SIZE_BATCH_TARGETS || path.size() + 1 > EXACT_SIZE_BATCH_PATH_BYTES - pathBytes) &&
            !flushBatch()) {
          useBatchSizes = false;
          break;
        }
        if (targetCount == 0) batchStart = i;
        char* targetPath = batch->paths + pathBytes;
        std::memcpy(targetPath, path.c_str(), path.size() + 1);
        batch->targets[targetCount] = {ZipFile::fnvHash64(targetPath, path.size()), static_cast<uint16_t>(path.size()),
                                       static_cast<uint16_t>(targetCount), targetPath};
        ++targetCount;
        pathBytes += path.size() + 1;
      }
      useBatchSizes = useBatchSizes && flushBatch();
    }
    LOG_DBG("BMC", "Exact ZIP size batch %s (scratch=%u free=%u maxAlloc=%u)",
            useBatchSizes ? "used" : "unavailable; using ZIP cursor", static_cast<unsigned>(sizeof(ExactSizeBatch)),
            static_cast<unsigned>(heap.free), static_cast<unsigned>(heap.largest));
  }

  uint32_t cumSize = 0;
  spineIn.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntryFrom(spineIn);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    constexpr size_t maxStoredCumulativeSize = std::numeric_limits<uint32_t>::max();
    if (itemSize > maxStoredCumulativeSize || cumSize > maxStoredCumulativeSize - itemSize) {
      LOG_ERR("BMC", "Spine cumulative size overflow for item %d (cumSize=%u, itemSize=%zu)", i, cumSize, itemSize);
      zip.close();
      closeBuildFiles();
      return false;
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntryTo(bookOut, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocIn.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntryFrom(tocIn);
    writeTocEntryTo(bookOut, tocEntry);
  }

  const bool written = bookOut.flush();

  // Explicit close() required: member variables persist beyond function scope
  closeBuildFiles();

  if (!written) {
    // A short write (card full/removed) would leave a truncated book.bin that
    // still passes the version check on load; remove it so the next open rebuilds.
    LOG_ERR("BMC", "Failed writing book.bin, removing truncated file");
    Storage.remove((cachePath + bookBinFile).c_str());
    return false;
  }

  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  return writeSpineEntryTo(file, entry);
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  return writeTocEntryTo(file, entry);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  if (passOut) {
    writeSpineEntryTo(*passOut, entry);
  } else {
    writeSpineEntry(spineFile, entry);
  }
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineIndex = tocSpineMemo.lookup(href, [&]() -> int16_t {
      spineFile.seek(0);
      for (int i = 0; i < spineCount; i++) {
        if (readSpineEntry(spineFile).href == href) return static_cast<int16_t>(i);
      }
      return -1;
    });
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level, spineIndex);
  if (passOut) {
    writeTocEntryTo(*passOut, entry);
  } else {
    writeTocEntry(tocFile, entry);
  }
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::exists(const std::string& cachePath) {
  return Storage.exists((cachePath + bookBinFile).c_str());
}

bool BookMetadataCache::load() {
  const auto bookBinPath = cachePath + bookBinFile;
  if (!Storage.openFileForRead("BMC", bookBinPath, bookFile)) {
    return false;
  }

  uint32_t magic;
  if (!serialization::tryReadPod(bookFile, magic)) {
    LOG_DBG("BMC", "Cache header is truncated");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }
  if (magic != BOOK_CACHE_MAGIC) {
    LOG_DBG("BMC", "Cache magic mismatch");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  uint8_t version;
  if (!serialization::tryReadPod(bookFile, version)) {
    LOG_DBG("BMC", "Cache version is missing");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  if (!serialization::tryReadPod(bookFile, lutOffset) || !serialization::tryReadPod(bookFile, spineCount) ||
      !serialization::tryReadPod(bookFile, tocCount) || !serialization::tryReadString(bookFile, coreMetadata.title) ||
      !serialization::tryReadString(bookFile, coreMetadata.author) ||
      !serialization::tryReadString(bookFile, coreMetadata.language) ||
      !serialization::tryReadString(bookFile, coreMetadata.coverItemHref) ||
      !serialization::tryReadString(bookFile, coreMetadata.textReferenceHref)) {
    LOG_DBG("BMC", "Cache metadata is truncated");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  cacheSpineCumulativeSizes();
  loaded = true;
  return true;
}

void BookMetadataCache::cacheSpineCumulativeSizes() {
  cumulativeSizes.reset();
  cumulativeSizeCount = 0;
  if (!cacheCumulativeSizes || spineCount == 0 || spineCount > MAX_CACHED_CUMULATIVE_SIZES) {
    return;
  }

  auto sizes = makeUniqueNoThrow<uint32_t[]>(spineCount);
  if (!sizes) {
    LOG_DBG("BMC", "Could not allocate %u-byte cumulative size cache; using SD lookups",
            static_cast<unsigned>(spineCount * sizeof(uint32_t)));
    return;
  }

  const uint32_t lutSize = (static_cast<uint32_t>(spineCount) + tocCount) * sizeof(uint32_t);
  if (!bookFile.seek(lutOffset + lutSize)) {
    LOG_DBG("BMC", "Could not seek to spine data for cumulative size cache; using SD lookups");
    return;
  }

  for (uint16_t i = 0; i < spineCount; ++i) {
    uint32_t hrefLen = 0;
    int16_t tocIndex = -1;
    if (!serialization::tryReadPod(bookFile, hrefLen) || !bookFile.seekCur(hrefLen) ||
        !serialization::tryReadPod(bookFile, sizes[i]) || !serialization::tryReadPod(bookFile, tocIndex)) {
      LOG_DBG("BMC", "Could not populate cumulative size cache; using SD lookups");
      return;
    }
  }

  cumulativeSizes = std::move(sizes);
  cumulativeSizeCount = spineCount;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

size_t BookMetadataCache::getSpineCumulativeSize(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineCumulativeSize called but cache not loaded");
    return 0;
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineCumulativeSize index %d out of range", index);
    return 0;
  }

  if (index < static_cast<int>(cumulativeSizeCount)) {
    return cumulativeSizes[index];
  }

  // Seek to spine LUT item, then read only the cumulative size field from the entry.
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);

  uint32_t hrefLen = 0;
  serialization::readPod(bookFile, hrefLen);
  bookFile.seekCur(hrefLen);

  uint32_t cumulativeSize = 0;
  serialization::readPod(bookFile, cumulativeSize);
  return static_cast<size_t>(cumulativeSize);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  return readSpineEntryFrom(file);
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const { return readTocEntryFrom(file); }
