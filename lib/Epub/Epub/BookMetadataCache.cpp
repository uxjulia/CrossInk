#include "BookMetadataCache.h"

#include <BufferedFile.h>
#include <Logging.h>
#include <Memory.h>
#include <OwnedFileSwap.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <cstring>
#include <iterator>
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
constexpr size_t MIN_SIZE_LOOKUP_TARGETS = 64;
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
constexpr size_t MAX_BUILD_ENTRY_STRING_BYTES = 16 * 1024;

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

template <typename F, typename T>
bool readPodChecked(F& file, T& value, const size_t fileSize, bool* const ioFailure = nullptr) {
  const size_t position = file.position();
  if (position > fileSize || sizeof(T) > fileSize - position) return false;
  if (static_cast<int>(file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T))) == static_cast<int>(sizeof(T))) {
    return true;
  }
  if (ioFailure) *ioFailure = true;
  return false;
}

template <typename F>
bool readStringChecked(F& file, std::string& value, const size_t fileSize, bool* const ioFailure = nullptr) {
  uint32_t length = 0;
  if (!readPodChecked(file, length, fileSize, ioFailure) || length > MAX_BUILD_ENTRY_STRING_BYTES ||
      length > value.max_size()) {
    return false;
  }
  const size_t position = file.position();
  if (position > fileSize || length > fileSize - position) return false;
  value.resize(length);
  if (length == 0) return true;
  if (static_cast<int>(file.read(reinterpret_cast<uint8_t*>(value.data()), length)) == static_cast<int>(length)) {
    return true;
  }
  if (ioFailure) *ioFailure = true;
  return false;
}

template <typename F>
bool readSpineEntryChecked(F& file, BookMetadataCache::SpineEntry& entry, const size_t fileSize,
                           bool* const ioFailure = nullptr) {
  return readStringChecked(file, entry.href, fileSize, ioFailure) &&
         readPodChecked(file, entry.cumulativeSize, fileSize, ioFailure) &&
         readPodChecked(file, entry.tocIndex, fileSize, ioFailure);
}

template <typename F>
bool readTocEntryChecked(F& file, BookMetadataCache::TocEntry& entry, const size_t fileSize,
                         bool* const ioFailure = nullptr) {
  return readStringChecked(file, entry.title, fileSize, ioFailure) &&
         readStringChecked(file, entry.href, fileSize, ioFailure) &&
         readStringChecked(file, entry.anchor, fileSize, ioFailure) &&
         readPodChecked(file, entry.level, fileSize, ioFailure) &&
         readPodChecked(file, entry.spineIndex, fileSize, ioFailure);
}
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  lowMemoryFailure = false;
  buildIoFailure = false;
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
  passOut.emplace(spineFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  // Explicit close() required: member variable persists beyond function scope
  const bool closed = spineFile.close();
  if (!flushed || !closed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
    buildIoFailure = true;
  }
  return flushed && closed;
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
    const size_t spineFileSize = spineFile.size();
    if (!spineFile.seek(0)) {
      buildIoFailure = true;
      tocFile.close();
      spineFile.close();
      return false;
    }
    for (int i = 0; i < spineCount; i++) {
      SpineEntry entry;
      if (!readSpineEntryChecked(spineFile, entry, spineFileSize)) {
        LOG_ERR("BMC", "Could not read spine tmp entry %d", i);
        buildIoFailure = true;
        spineHrefIndex.resetStorage();
        spineHrefIndexArena.release();
        tocFile.close();
        spineFile.close();
        return false;
      }
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
    if (spineFile.position() != spineFileSize || !spineFile.seek(0)) {
      LOG_ERR("BMC", "Spine tmp file has an invalid size or cannot be rewound");
      buildIoFailure = true;
      spineHrefIndex.resetStorage();
      spineHrefIndexArena.release();
      tocFile.close();
      spineFile.close();
      return false;
    }
    useSpineHrefIndex = true;
  } else {
    useSpineHrefIndex = false;
  }

  passOut.emplace(tocFile, BUILD_IO_BUFFER_SIZE);
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
  const bool closed = tocFile.close();
  const bool spineClosed = spineFile.close();

  spineHrefIndex.resetStorage();
  spineHrefIndexArena.release();
  useSpineHrefIndex = false;

  if (!flushed || !closed || !spineClosed) {
    buildIoFailure = true;
  }
  return flushed && closed && spineClosed;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  return !buildIoFailure;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  const std::string finalPath = cachePath + bookBinFile;
  const std::string staged = finalPath + ".tmp";
  if (!OwnedFileSwap::recover(finalPath) || !Storage.openFileForWrite("BMC", staged, bookFile)) {
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

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_MAGIC) + sizeof(BOOK_CACHE_VERSION) +
                                   /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  // Buffered streams for the whole build: every access below is sequential per
  // file, but interleaved ACROSS files, which thrashes SdFat's single shared
  // sector cache when unbuffered (one 512B SD transaction per 4-byte pod --
  // measured 31s for a 1,732-spine omnibus). Three 4KB buffers, freed on return.
  serialization::BufferedFileWriter bookOut(bookFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader spineIn(spineFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader tocIn(tocFile, BUILD_IO_BUFFER_SIZE);
  const size_t spineInputSize = spineFile.size();
  const size_t tocInputSize = tocFile.size();
  auto closeBuildFiles = [&]() {
    const bool flushed = bookOut.flush();
    const bool closed = bookFile.close();
    const bool spineClosed = spineFile.close();
    const bool tocClosed = tocFile.close();
    return flushed && closed && spineClosed && tocClosed;
  };
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
  bool inputOk = spineIn.seek(0);
  if (!inputOk) buildIoFailure = true;
  for (int i = 0; i < spineCount; i++) {
    if (!inputOk) break;
    const uint32_t pos = spineIn.position();
    SpineEntry entry;
    if (!readSpineEntryChecked(spineIn, entry, spineInputSize)) {
      inputOk = false;
      buildIoFailure = true;
      break;
    }
    if (tryExactSizeBatch) tryExactSizeBatch = sizeBatchPlan.add(entry.href);
    serialization::writePod(bookOut, pos + lutOffset + lutSize);
  }
  // Total size of the spine tmp file: entries land in book.bin after the toc LUT
  // and the full spine block, so toc LUT positions are offset by it.
  const auto spineBytes = static_cast<uint32_t>(spineIn.position());
  if (!inputOk || spineIn.position() != spineInputSize) {
    LOG_ERR("BMC", "Spine tmp file is truncated or contains trailing data");
    closeBuildFiles();
    return false;
  }

  // Loop through toc entries, writing LUT positions
  inputOk = tocIn.seek(0);
  if (!inputOk) buildIoFailure = true;
  for (int i = 0; i < tocCount; i++) {
    if (!inputOk) break;
    const uint32_t pos = tocIn.position();
    TocEntry entry;
    if (!readTocEntryChecked(tocIn, entry, tocInputSize)) {
      inputOk = false;
      buildIoFailure = true;
      break;
    }
    serialization::writePod(bookOut, pos + lutOffset + lutSize + spineBytes);
  }
  if (!inputOk || tocIn.position() != tocInputSize) {
    LOG_ERR("BMC", "TOC tmp file is truncated or contains trailing data");
    closeBuildFiles();
    return false;
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
  inputOk = tocIn.seek(0);
  if (!inputOk) buildIoFailure = true;
  for (int j = 0; j < tocCount; j++) {
    if (!inputOk) break;
    TocEntry tocEntry;
    if (!readTocEntryChecked(tocIn, tocEntry, tocInputSize)) {
      inputOk = false;
      buildIoFailure = true;
      break;
    }
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }
  if (!inputOk || tocIn.position() != tocInputSize) {
    LOG_ERR("BMC", "TOC tmp file failed while building spine mappings");
    closeBuildFiles();
    return false;
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
    size_t batchCapacity = std::min<size_t>(spineCount, MAX_SIZE_LOOKUP_TARGETS);
    bool haveBatchScratch = spineSizes.resize(spineCount);
    if (haveBatchScratch) {
      while (batchCapacity >= MIN_SIZE_LOOKUP_TARGETS && !targets.resize(batchCapacity)) {
        if (batchCapacity == MIN_SIZE_LOOKUP_TARGETS) {
          batchCapacity = 0;
          break;
        }
        const size_t smaller = batchCapacity / 2;
        batchCapacity = std::max(MIN_SIZE_LOOKUP_TARGETS, smaller);
      }
      haveBatchScratch = targets.capacity() >= MIN_SIZE_LOOKUP_TARGETS;
    }

    if (haveBatchScratch) {
      size_t targetCount = 0;
      int matched = 0;
      inputOk = spineIn.seek(0);
      if (!inputOk) buildIoFailure = true;
      for (int i = 0; i < spineCount; i++) {
        if (!inputOk) break;
        SpineEntry entry;
        if (!readSpineEntryChecked(spineIn, entry, spineInputSize)) {
          inputOk = false;
          buildIoFailure = true;
          break;
        }
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
      if (!inputOk || spineIn.position() != spineInputSize) {
        LOG_ERR("BMC", "Spine tmp file failed during size lookup");
        zip.close();
        closeBuildFiles();
        return false;
      }
      LOG_DBG("BMC", "Batch lookup matched %d/%d spine items (batch=%u)", matched, spineCount,
              static_cast<unsigned>(batchCapacity));

      useBatchSizes = true;
    } else {
      // Keep the existing per-entry ZIP lookup as a correctness-preserving
      // fallback when the result vector or even the smallest scratch batch
      // cannot be allocated. This may take longer, but avoids turning memory
      // pressure into a failed book open.
      LOG_DBG("BMC", "Batch size lookup unavailable; using ZIP cursor for %u spine items", spineCount);
    }
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
      size_t parsedSpineCount = 0;
      bool exactBatchSupported = true;
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

      inputOk = spineIn.seek(0);
      if (!inputOk) buildIoFailure = true;
      for (size_t i = 0; i < spineCount; ++i) {
        if (!inputOk) break;
        SpineEntry entry;
        if (!readSpineEntryChecked(spineIn, entry, spineInputSize)) {
          inputOk = false;
          buildIoFailure = true;
          break;
        }
        ++parsedSpineCount;
        const std::string path = FsHelpers::normalisePath(entry.href);
        if (path.size() + 1 > EXACT_SIZE_BATCH_PATH_BYTES || path.find('\0') != std::string::npos) {
          useBatchSizes = false;
          exactBatchSupported = false;
          break;
        }
        if ((targetCount == EXACT_SIZE_BATCH_TARGETS || path.size() + 1 > EXACT_SIZE_BATCH_PATH_BYTES - pathBytes) &&
            !flushBatch()) {
          useBatchSizes = false;
          exactBatchSupported = false;
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
      for (; inputOk && !exactBatchSupported && parsedSpineCount < spineCount; ++parsedSpineCount) {
        SpineEntry ignored;
        if (!readSpineEntryChecked(spineIn, ignored, spineInputSize)) inputOk = false;
      }
      if (!inputOk || spineIn.position() != spineInputSize) {
        LOG_ERR("BMC", "Spine tmp file failed during exact size lookup");
        zip.close();
        closeBuildFiles();
        return false;
      }
      useBatchSizes = useBatchSizes && exactBatchSupported && flushBatch();
    }
    LOG_DBG("BMC", "Exact ZIP size batch %s (scratch=%u free=%u maxAlloc=%u)",
            useBatchSizes ? "used" : "unavailable; using ZIP cursor", static_cast<unsigned>(sizeof(ExactSizeBatch)),
            static_cast<unsigned>(heap.free), static_cast<unsigned>(heap.largest));
  }

  uint32_t cumSize = 0;
  inputOk = spineIn.seek(0);
  if (!inputOk) buildIoFailure = true;
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    if (!inputOk) break;
    SpineEntry spineEntry;
    if (!readSpineEntryChecked(spineIn, spineEntry, spineInputSize)) {
      inputOk = false;
      buildIoFailure = true;
      break;
    }

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
  if (!inputOk || spineIn.position() != spineInputSize) {
    LOG_ERR("BMC", "Spine tmp file failed while writing metadata entries");
    zip.close();
    closeBuildFiles();
    return false;
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  inputOk = tocIn.seek(0);
  if (!inputOk) buildIoFailure = true;
  for (int i = 0; i < tocCount; i++) {
    if (!inputOk) break;
    TocEntry tocEntry;
    if (!readTocEntryChecked(tocIn, tocEntry, tocInputSize)) {
      inputOk = false;
      buildIoFailure = true;
      break;
    }
    writeTocEntryTo(bookOut, tocEntry);
  }
  if (!inputOk || tocIn.position() != tocInputSize) {
    LOG_ERR("BMC", "TOC tmp file failed while writing metadata entries");
    closeBuildFiles();
    return false;
  }

  const bool buildFilesClosed = closeBuildFiles();
  if (buildIoFailure || !buildFilesClosed || !OwnedFileSwap::publish(finalPath, staged)) {
    LOG_ERR("BMC", "Failed to publish book metadata");
    Storage.remove(staged.c_str());
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

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntryTo(*passOut, entry);
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
      if (!spineFile.seek(0)) {
        LOG_ERR("BMC", "Could not seek spine tmp file while matching TOC entry");
        buildIoFailure = true;
        return -1;
      }
      const size_t spineFileSize = spineFile.size();
      for (int i = 0; i < spineCount; i++) {
        SpineEntry entry;
        if (!readSpineEntryChecked(spineFile, entry, spineFileSize)) {
          LOG_ERR("BMC", "Could not read spine tmp entry %d while matching TOC", i);
          buildIoFailure = true;
          return -1;
        }
        if (entry.href == href) return static_cast<int16_t>(i);
      }
      if (spineFile.position() != spineFileSize) buildIoFailure = true;
      return -1;
    });
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  if (buildIoFailure) return;

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level, spineIndex);
  writeTocEntryTo(*passOut, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::exists(const std::string& cachePath) {
  const auto path = cachePath + bookBinFile;
  return OwnedFileSwap::recover(path) && Storage.exists(path.c_str());
}

namespace {
// A book.bin metadata field is length-prefixed and ends before the LUT.
// Check it against that boundary before resizing, not after a short read.
bool readMetadataField(HalFile& file, std::string& value, const size_t metadataEnd) {
  const size_t start = file.position();
  uint32_t length = 0;
  if (start > metadataEnd || metadataEnd - start < sizeof(length) || !serialization::tryReadPod(file, length) ||
      length > metadataEnd - start - sizeof(length) || length > value.max_size() ||
      length > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  value.resize(length);
  return length == 0 || file.read(value.data(), length) == static_cast<int>(length);
}
}  // namespace

bool BookMetadataCache::load() {
  loaded = false;
  cumulativeSizes.reset();
  cumulativeSizeCount = 0;
  if (bookFile) bookFile.close();

  const auto bookBinPath = cachePath + bookBinFile;
  if (!OwnedFileSwap::recover(bookBinPath) || !Storage.openFileForRead("BMC", bookBinPath, bookFile)) {
    return false;
  }

  const auto rejectCache = [&] {
    // Only the derived metadata cache is removed; preserve progress and other
    // user files so the reader can rebuild the metadata normally.
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  };
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(bookFile, magic) || magic != BOOK_CACHE_MAGIC) {
    LOG_DBG("BMC", "Cache magic missing or invalid");
    return rejectCache();
  }
  if (!serialization::tryReadPod(bookFile, version) || version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    return rejectCache();
  }
  if (!serialization::tryReadPod(bookFile, lutOffset) || !serialization::tryReadPod(bookFile, spineCount) ||
      !serialization::tryReadPod(bookFile, tocCount)) {
    LOG_DBG("BMC", "Cache header is truncated");
    return rejectCache();
  }

  const size_t fileSize = bookFile.size();
  const size_t lutSize = (static_cast<size_t>(spineCount) + tocCount) * sizeof(uint32_t);
  if (lutOffset > fileSize || lutSize > fileSize - lutOffset) {
    LOG_DBG("BMC", "Cache lookup table is outside the file");
    return rejectCache();
  }
  std::string* const fields[] = {&coreMetadata.title, &coreMetadata.author, &coreMetadata.language,
                                 &coreMetadata.coverItemHref, &coreMetadata.textReferenceHref};
  const size_t metadataStart = bookFile.position();
  if (lutOffset < metadataStart || lutOffset - metadataStart < std::size(fields) * sizeof(uint32_t)) {
    LOG_DBG("BMC", "Cache metadata is missing required lengths");
    return rejectCache();
  }
  for (size_t i = 0; i < std::size(fields); ++i) {
    // A field cannot consume even the required length prefixes of later fields.
    const size_t fieldEnd = lutOffset - (std::size(fields) - i - 1) * sizeof(uint32_t);
    if (!readMetadataField(bookFile, *fields[i], fieldEnd)) {
      LOG_DBG("BMC", "Cache metadata is truncated or inconsistent");
      return rejectCache();
    }
  }
  if (bookFile.position() != lutOffset) {
    LOG_DBG("BMC", "Cache metadata does not end at the lookup table");
    return rejectCache();
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

  for (uint16_t i = 0; i < spineCount; ++i) {
    bool ioFailure = false;
    size_t start = 0;
    size_t end = 0;
    if (!readEntryBounds(true, i, start, end, ioFailure) || !bookFile.seek(start)) {
      LOG_DBG("BMC", "Could not locate spine entry for cumulative size cache; using SD lookups");
      return;
    }
    uint32_t hrefLen = 0;
    int16_t tocIndex = -1;
    if (!readPodChecked(bookFile, hrefLen, end, &ioFailure) || hrefLen > MAX_BUILD_ENTRY_STRING_BYTES ||
        hrefLen > end - bookFile.position() || !bookFile.seekCur(hrefLen) ||
        !readPodChecked(bookFile, sizes[i], end, &ioFailure) || !readPodChecked(bookFile, tocIndex, end, &ioFailure) ||
        bookFile.position() != end) {
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

  bool ioFailure = false;
  size_t start = 0;
  size_t end = 0;
  if (!readEntryBounds(true, index, start, end, ioFailure)) {
    invalidateReadCache(!ioFailure);
    return {};
  }
  if (!bookFile.seek(start)) {
    invalidateReadCache(false);
    return {};
  }
  SpineEntry entry;
  if (!readSpineEntryChecked(bookFile, entry, end, &ioFailure) || bookFile.position() != end) {
    invalidateReadCache(!ioFailure);
    return {};
  }
  return entry;
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

  bool ioFailure = false;
  size_t start = 0;
  size_t end = 0;
  if (!readEntryBounds(true, index, start, end, ioFailure)) {
    invalidateReadCache(!ioFailure);
    return 0;
  }
  if (!bookFile.seek(start)) {
    invalidateReadCache(false);
    return 0;
  }
  uint32_t hrefLen = 0;
  if (!readPodChecked(bookFile, hrefLen, end, &ioFailure) || hrefLen > MAX_BUILD_ENTRY_STRING_BYTES ||
      hrefLen > end - bookFile.position()) {
    invalidateReadCache(!ioFailure);
    return 0;
  }
  if (!bookFile.seekCur(hrefLen)) {
    invalidateReadCache(false);
    return 0;
  }
  uint32_t cumulativeSize = 0;
  int16_t tocIndex = -1;
  if (!readPodChecked(bookFile, cumulativeSize, end, &ioFailure) ||
      !readPodChecked(bookFile, tocIndex, end, &ioFailure) || bookFile.position() != end) {
    invalidateReadCache(!ioFailure);
    return 0;
  }
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

  bool ioFailure = false;
  size_t start = 0;
  size_t end = 0;
  if (!readEntryBounds(false, index, start, end, ioFailure)) {
    invalidateReadCache(!ioFailure);
    return {};
  }
  if (!bookFile.seek(start)) {
    invalidateReadCache(false);
    return {};
  }
  TocEntry entry;
  if (!readTocEntryChecked(bookFile, entry, end, &ioFailure) || bookFile.position() != end) {
    invalidateReadCache(!ioFailure);
    return {};
  }
  return entry;
}

bool BookMetadataCache::readEntryBounds(const bool spineEntry, const int index, size_t& start, size_t& end,
                                        bool& ioFailure) {
  const size_t fileSize = bookFile.size();
  const size_t lutStart = lutOffset;
  const size_t spineLutSize = static_cast<size_t>(spineCount) * sizeof(uint32_t);
  const size_t tocLutSize = static_cast<size_t>(tocCount) * sizeof(uint32_t);
  if (lutStart > fileSize || spineLutSize > fileSize - lutStart || tocLutSize > fileSize - lutStart - spineLutSize) {
    return false;
  }
  const size_t tableEnd = lutStart + spineLutSize + tocLutSize;
  const size_t slot = spineEntry ? lutStart + static_cast<size_t>(index) * sizeof(uint32_t)
                                 : lutStart + spineLutSize + static_cast<size_t>(index) * sizeof(uint32_t);
  if (!bookFile.seek(slot)) {
    ioFailure = true;
    return false;
  }
  uint32_t position = 0;
  if (!readPodChecked(bookFile, position, fileSize, &ioFailure)) return false;
  start = position;

  size_t nextSlot = fileSize;
  if (spineEntry && index + 1 < spineCount) {
    nextSlot = slot + sizeof(uint32_t);
  } else if (spineEntry && tocCount > 0) {
    nextSlot = lutStart + spineLutSize;
  } else if (!spineEntry && index + 1 < tocCount) {
    nextSlot = slot + sizeof(uint32_t);
  } else {
    end = fileSize;
    return start >= tableEnd && start < end;
  }

  if (!bookFile.seek(nextSlot)) {
    ioFailure = true;
    return false;
  }
  uint32_t nextPosition = 0;
  if (!readPodChecked(bookFile, nextPosition, fileSize, &ioFailure)) return false;
  end = nextPosition;
  return start >= tableEnd && start < end && end <= fileSize;
}

void BookMetadataCache::invalidateReadCache(const bool discardCache) {
  LOG_ERR("BMC", "Metadata cache read failed, disabling cache");
  loaded = false;
  cumulativeSizes.reset();
  cumulativeSizeCount = 0;
  const bool closed = !bookFile || bookFile.close();
  if (discardCache && closed && !Storage.remove((cachePath + bookBinFile).c_str())) {
    LOG_ERR("BMC", "Could not remove invalid metadata cache");
  }
}
