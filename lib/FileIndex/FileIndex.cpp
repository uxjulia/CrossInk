#include "FileIndex.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <NaturalSort.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* INDEX_ROOT = "/.crosspoint";
constexpr const char* INDEX_DIR = "/.crosspoint/fileindex";
constexpr char MAGIC[4] = {'C', 'P', 'F', 'I'};
constexpr uint8_t INDEX_VERSION = 1;
constexpr size_t CHUNK_ENTRIES = 64;
constexpr size_t NAME_BUF_SIZE = FileIndex::MAX_NAME + 1;
constexpr uint8_t SECTION_DIR = 1;
constexpr uint8_t SECTION_FILE = 2;
constexpr size_t MAX_TIE_SEGMENT = 64;
constexpr uint32_t FNV32_BASIS = 2166136261u;

uint32_t fnv1a32(const void* data, size_t len, uint32_t hash) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}

uint64_t fnv1a64(const char* s) {
  uint64_t hash = 1469598103934665603ULL;
  while (*s) {
    hash ^= static_cast<uint8_t>(*s++);
    hash *= 1099511628211ULL;
  }
  return hash;
}

void maybeYield(uint32_t& counter) {
  if ((++counter & 0xFF) == 0) delay(1);
}
}  // namespace

struct FileIndex::BuildState {
  HalFile idxTmp;
  HalFile runsOut;
  char tmpPath[64] = {0};
  char runsPathA[48] = {0};
  char runsPathB[48] = {0};
  char tiePathA[48] = {0};
  char tiePathB[48] = {0};
  const char* finalRunsPath = nullptr;
  std::unique_ptr<RunRecord[]> chunk;
  size_t chunkUsed = 0;
  uint32_t runCount = 0;
  uint32_t blobLen = 0;
  std::unique_ptr<char[]> nameA;
  std::unique_ptr<char[]> nameB;
  uint32_t segment[MAX_TIE_SEGMENT] = {0};
  uint32_t yieldCounter = 0;
};

bool FileIndex::open(const char* dirPath, AcceptFn accept) {
  close();

  if (!nameBuf) nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("FIDX", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUF_SIZE));
    return false;
  }

  uint32_t signature = 0;
  uint32_t dirs = 0;
  uint32_t files = 0;
  if (!scanDirectory(dirPath, accept, signature, dirs, files)) {
    return false;
  }

  if (loadExisting(dirPath, signature, dirs, files)) {
    LOG_DBG("FIDX", "index valid for %s (%u dirs, %u files)", dirPath, hdr.dirCount, hdr.fileCount);
    return true;
  }

  LOG_INF("FIDX", "building index for %s (%u dirs, %u files)", dirPath, dirs, files);
  return build(dirPath, accept, signature, dirs, files);
}

void FileIndex::close() {
  if (idxFile) idxFile.close();
  opened = false;
  offsetsCacheFirst = SIZE_MAX;
  memset(&hdr, 0, sizeof(hdr));
}

bool FileIndex::scanDirectory(const char* dirPath, AcceptFn accept, uint32_t& signature, uint32_t& dirs,
                              uint32_t& files) {
  auto root = Storage.open(dirPath);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }
  root.rewindDirectory();

  uint32_t sig = FNV32_BASIS;
  dirs = 0;
  files = 0;
  uint32_t yieldCounter = 0;

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(nameBuf.get(), NAME_BUF_SIZE);
    const bool isDir = file.isDirectory();
    if (!accept(nameBuf.get(), isDir)) {
      file.close();
      continue;
    }

    const uint8_t dirByte = isDir ? 1 : 0;
    sig = fnv1a32(nameBuf.get(), strlen(nameBuf.get()), sig);
    sig = fnv1a32(&dirByte, sizeof(dirByte), sig);

    if (isDir) {
      dirs++;
    } else {
      files++;
    }
    file.close();
    maybeYield(yieldCounter);
  }
  root.close();

  signature = sig;
  return true;
}

bool FileIndex::loadExisting(const char* dirPath, uint32_t signature, uint32_t dirs, uint32_t files) {
  snprintf(idxPath, sizeof(idxPath), "%s/%016llx.idx", INDEX_DIR, static_cast<unsigned long long>(fnv1a64(dirPath)));

  auto file = Storage.open(idxPath);
  if (!file) return false;

  IndexHeader h{};
  bool valid = file.read(&h, sizeof(h)) == static_cast<int>(sizeof(h)) && memcmp(h.magic, MAGIC, sizeof(MAGIC)) == 0 &&
               h.version == INDEX_VERSION && h.dirSignature == signature && h.dirCount == dirs &&
               h.fileCount == files && h.pathLen == strlen(dirPath) && h.pathLen < NAME_BUF_SIZE;

  if (valid) {
    valid = file.read(nameBuf.get(), h.pathLen) == static_cast<int>(h.pathLen) &&
            memcmp(nameBuf.get(), dirPath, h.pathLen) == 0;
  }

  if (valid) {
    const uint64_t expectedSize =
        static_cast<uint64_t>(h.offsetsStart) + static_cast<uint64_t>(h.dirCount + h.fileCount) * sizeof(uint32_t);
    valid = file.fileSize64() == expectedSize;
  }

  if (!valid) {
    file.close();
    return false;
  }

  hdr = h;
  idxFile = std::move(file);
  opened = true;
  return true;
}

void FileIndex::makeKey(RunRecord& rec, bool isDir, const char* name) const {
  memset(rec.key, 0, sizeof(rec.key));
  rec.key[0] = isDir ? SECTION_DIR : SECTION_FILE;
  FsHelpers::naturalSortKey(name, rec.key + 1, sizeof(rec.key) - 1);
}

bool FileIndex::flushChunk(BuildState& bs) {
  if (bs.chunkUsed == 0) return true;

  std::sort(bs.chunk.get(), bs.chunk.get() + bs.chunkUsed, [](const RunRecord& a, const RunRecord& b) {
    const int cmp = memcmp(a.key, b.key, sizeof(a.key));
    if (cmp != 0) return cmp < 0;
    return a.blobOffset < b.blobOffset;
  });

  const size_t bytes = bs.chunkUsed * sizeof(RunRecord);
  if (bs.runsOut.write(bs.chunk.get(), bytes) != bytes) {
    LOG_ERR("FIDX", "run write failed");
    return false;
  }
  bs.chunkUsed = 0;
  bs.runCount++;
  return true;
}

bool FileIndex::build(const char* dirPath, AcceptFn accept, uint32_t signature, uint32_t dirs, uint32_t files) {
  if (!Storage.ensureDirectoryExists(INDEX_ROOT) || !Storage.ensureDirectoryExists(INDEX_DIR)) {
    LOG_ERR("FIDX", "cannot create %s", INDEX_DIR);
    return false;
  }

  auto bsPtr = makeUniqueNoThrow<BuildState>();
  if (!bsPtr) {
    LOG_ERR("FIDX", "build state alloc failed (%u bytes)", static_cast<unsigned>(sizeof(BuildState)));
    return false;
  }
  BuildState& bs = *bsPtr;

  snprintf(bs.tmpPath, sizeof(bs.tmpPath), "%s.tmp", idxPath);
  snprintf(bs.runsPathA, sizeof(bs.runsPathA), "%s/runs.a", INDEX_DIR);
  snprintf(bs.runsPathB, sizeof(bs.runsPathB), "%s/runs.b", INDEX_DIR);
  snprintf(bs.tiePathA, sizeof(bs.tiePathA), "%s/tie.a", INDEX_DIR);
  snprintf(bs.tiePathB, sizeof(bs.tiePathB), "%s/tie.b", INDEX_DIR);

  bs.chunk = makeUniqueNoThrow<RunRecord[]>(CHUNK_ENTRIES);
  bs.nameA = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  bs.nameB = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!bs.chunk || !bs.nameA || !bs.nameB) {
    LOG_ERR("FIDX", "build scratch alloc failed (%u bytes)",
            static_cast<unsigned>(CHUNK_ENTRIES * sizeof(RunRecord) + 2 * NAME_BUF_SIZE));
    return false;
  }

  auto cleanupScratch = [&bs]() {
    Storage.remove(bs.tmpPath);
    Storage.remove(bs.runsPathA);
    Storage.remove(bs.runsPathB);
    Storage.remove(bs.tiePathA);
    Storage.remove(bs.tiePathB);
  };

  bs.idxTmp = Storage.open(bs.tmpPath, O_RDWR | O_CREAT | O_TRUNC);
  bs.runsOut = Storage.open(bs.runsPathA, O_RDWR | O_CREAT | O_TRUNC);
  if (!bs.idxTmp || !bs.runsOut) {
    LOG_ERR("FIDX", "cannot open scratch files");
    cleanupScratch();
    return false;
  }

  const uint16_t pathLen = static_cast<uint16_t>(strlen(dirPath));
  const uint32_t blobStart = sizeof(IndexHeader) + pathLen;

  IndexHeader newHdr{};
  bool ok = bs.idxTmp.write(&newHdr, sizeof(newHdr)) == sizeof(newHdr) && bs.idxTmp.write(dirPath, pathLen) == pathLen;

  uint32_t sig = FNV32_BASIS;
  uint32_t dirCount = 0;
  uint32_t fileCount = 0;

  if (ok) {
    auto root = Storage.open(dirPath);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      ok = false;
    } else {
      root.rewindDirectory();
      for (auto file = root.openNextFile(); ok && file; file = root.openNextFile()) {
        file.getName(nameBuf.get(), NAME_BUF_SIZE);
        const bool isDir = file.isDirectory();
        if (!accept(nameBuf.get(), isDir)) {
          file.close();
          continue;
        }

        const uint8_t dirByte = isDir ? 1 : 0;
        const uint16_t nameLen = static_cast<uint16_t>(strlen(nameBuf.get()));
        sig = fnv1a32(nameBuf.get(), nameLen, sig);
        sig = fnv1a32(&dirByte, sizeof(dirByte), sig);
        if (isDir) {
          dirCount++;
        } else {
          fileCount++;
        }

        RecordHeader rec{};
        rec.flags = isDir ? 1 : 0;
        rec.nameLen = nameLen;

        RunRecord run;
        run.blobOffset = blobStart + bs.blobLen;
        makeKey(run, isDir, nameBuf.get());

        ok = bs.idxTmp.write(&rec, sizeof(rec)) == sizeof(rec) && bs.idxTmp.write(nameBuf.get(), nameLen) == nameLen;
        bs.blobLen += sizeof(rec) + nameLen;

        bs.chunk[bs.chunkUsed++] = run;
        if (bs.chunkUsed == CHUNK_ENTRIES) ok = ok && flushChunk(bs);
        file.close();
        maybeYield(bs.yieldCounter);
      }
      root.close();
    }
  }
  ok = ok && sig == signature && dirCount == dirs && fileCount == files;
  ok = ok && flushChunk(bs);
  bs.runsOut.flush();
  bs.runsOut.close();

  const uint32_t recordCount = dirCount + fileCount;
  ok = ok && mergeRuns(bs, recordCount);
  ok = ok && writeOffsets(bs, recordCount);

  if (ok) {
    memcpy(newHdr.magic, MAGIC, sizeof(MAGIC));
    newHdr.version = INDEX_VERSION;
    newHdr.pathLen = pathLen;
    newHdr.dirSignature = sig;
    newHdr.dirCount = dirCount;
    newHdr.fileCount = fileCount;
    newHdr.blobStart = blobStart;
    newHdr.blobLen = bs.blobLen;
    newHdr.offsetsStart = blobStart + bs.blobLen;
    ok = bs.idxTmp.seek(0) && bs.idxTmp.write(&newHdr, sizeof(newHdr)) == sizeof(newHdr);
    bs.idxTmp.flush();
  }
  bs.idxTmp.close();

  if (!ok) {
    LOG_ERR("FIDX", "index build failed for %s", dirPath);
    cleanupScratch();
    return false;
  }

  Storage.remove(idxPath);
  if (!Storage.rename(bs.tmpPath, idxPath)) {
    LOG_ERR("FIDX", "rename to %s failed", idxPath);
    cleanupScratch();
    return false;
  }
  Storage.remove(bs.runsPathA);
  Storage.remove(bs.runsPathB);
  Storage.remove(bs.tiePathA);
  Storage.remove(bs.tiePathB);

  idxFile = Storage.open(idxPath);
  if (!idxFile) {
    LOG_ERR("FIDX", "reopen failed: %s", idxPath);
    return false;
  }
  hdr = newHdr;
  opened = true;
  LOG_INF("FIDX", "index built: %u dirs, %u files", dirCount, fileCount);
  return true;
}

bool FileIndex::mergeRuns(BuildState& bs, uint32_t recordCount) {
  if (recordCount == 0) {
    bs.finalRunsPath = bs.runsPathA;
    return true;
  }

  const char* inPath = bs.runsPathA;
  const char* outPath = bs.runsPathB;
  uint32_t runLen = CHUNK_ENTRIES;
  uint32_t runCount = bs.runCount;

  while (runCount > 1) {
    auto in = Storage.open(inPath);
    auto out = Storage.open(outPath, O_RDWR | O_CREAT | O_TRUNC);
    if (!in || !out) {
      LOG_ERR("FIDX", "merge pass open failed");
      if (in) in.close();
      if (out) out.close();
      return false;
    }

    auto readRunRecordAt = [&in](uint32_t index, RunRecord& rec) {
      const size_t pos = static_cast<size_t>(index) * sizeof(RunRecord);
      return in.seek(pos) && in.read(&rec, sizeof(rec)) == static_cast<int>(sizeof(rec));
    };

    bool ok = true;
    uint32_t newRunCount = 0;
    for (uint32_t run = 0; ok && run < runCount; run += 2) {
      const uint32_t startA = run * runLen;
      const uint32_t lenA = std::min(runLen, recordCount - startA);

      if (run + 1 >= runCount) {
        ok = in.seek(static_cast<size_t>(startA) * sizeof(RunRecord));
        uint32_t left = lenA;
        while (ok && left > 0) {
          const uint32_t batch = std::min<uint32_t>(left, CHUNK_ENTRIES);
          const size_t bytes = batch * sizeof(RunRecord);
          ok = in.read(bs.chunk.get(), bytes) == static_cast<int>(bytes) && out.write(bs.chunk.get(), bytes) == bytes;
          left -= batch;
          maybeYield(bs.yieldCounter);
        }
        newRunCount++;
        break;
      }

      const uint32_t startB = startA + lenA;
      const uint32_t lenB = std::min(runLen, recordCount - startB);
      uint32_t ia = 0;
      uint32_t ib = 0;
      bool haveA = false;
      bool haveB = false;
      RunRecord recA{};
      RunRecord recB{};

      while (ok && (ia < lenA || ib < lenB)) {
        if (!haveA && ia < lenA) {
          ok = readRunRecordAt(startA + ia, recA);
          haveA = ok;
        }
        if (ok && !haveB && ib < lenB) {
          ok = readRunRecordAt(startB + ib, recB);
          haveB = ok;
        }
        if (!ok) break;

        bool takeA;
        if (haveA && haveB) {
          const int cmp = memcmp(recA.key, recB.key, sizeof(recA.key));
          takeA = cmp < 0 || (cmp == 0 && recA.blobOffset <= recB.blobOffset);
        } else {
          takeA = haveA;
        }

        const RunRecord& rec = takeA ? recA : recB;
        ok = out.write(&rec, sizeof(rec)) == sizeof(rec);
        if (takeA) {
          haveA = false;
          ia++;
        } else {
          haveB = false;
          ib++;
        }
        maybeYield(bs.yieldCounter);
      }
      newRunCount++;
    }

    in.close();
    out.flush();
    out.close();
    if (!ok) return false;

    std::swap(inPath, outPath);
    runLen *= 2;
    runCount = newRunCount;
  }

  bs.finalRunsPath = inPath;
  return true;
}

bool FileIndex::readNameAt(HalFile& file, uint32_t recordOffset, char* out, size_t cap) {
  RecordHeader rec{};
  if (!file.seek(recordOffset) || file.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;
  const size_t n = std::min<size_t>(rec.nameLen, cap - 1);
  if (file.read(out, n) != static_cast<int>(n)) return false;
  out[n] = '\0';
  return true;
}

bool FileIndex::writeOffsets(BuildState& bs, uint32_t recordCount) {
  auto run = Storage.open(bs.finalRunsPath);
  if (!run && recordCount > 0) {
    LOG_ERR("FIDX", "cannot open final run");
    return false;
  }

  uint32_t* segment = bs.segment;
  bool ok = true;
  uint32_t appendPos = bs.idxTmp.position();

  auto readRunRecordAt = [&](uint32_t index, RunRecord& out) {
    return run.seek(static_cast<size_t>(index) * sizeof(RunRecord)) &&
           run.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
  };

  auto readTieOffsetAt = [](HalFile& file, uint32_t index, uint32_t& out) {
    return file.seek(static_cast<size_t>(index) * sizeof(uint32_t)) &&
           file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
  };

  auto appendOffset = [&](uint32_t offset) {
    if (!bs.idxTmp.seek(appendPos) || bs.idxTmp.write(&offset, sizeof(offset)) != sizeof(offset)) return false;
    appendPos += sizeof(offset);
    return true;
  };

  auto offsetComesBefore = [&](uint32_t left, uint32_t right) {
    if (!readNameAt(bs.idxTmp, left, bs.nameA.get(), NAME_BUF_SIZE) ||
        !readNameAt(bs.idxTmp, right, bs.nameB.get(), NAME_BUF_SIZE)) {
      ok = false;
      return false;
    }
    const int cmp = FsHelpers::naturalCompare(bs.nameA.get(), bs.nameB.get());
    return cmp < 0 || (cmp == 0 && left <= right);
  };

  auto sortSegmentByName = [&](size_t segmentLen) {
    bs.idxTmp.flush();
    for (size_t i = 1; ok && i < segmentLen; i++) {
      const uint32_t candidate = segment[i];
      ok = readNameAt(bs.idxTmp, candidate, bs.nameA.get(), NAME_BUF_SIZE);
      size_t j = i;
      while (ok && j > 0) {
        ok = readNameAt(bs.idxTmp, segment[j - 1], bs.nameB.get(), NAME_BUF_SIZE);
        const int cmp = ok ? FsHelpers::naturalCompare(bs.nameB.get(), bs.nameA.get()) : 0;
        if (!ok || cmp < 0 || (cmp == 0 && segment[j - 1] <= candidate)) break;
        segment[j] = segment[j - 1];
        j--;
      }
      segment[j] = candidate;
    }
    return ok;
  };

  auto appendSmallTieGroup = [&](uint32_t groupStart, uint32_t groupLen) {
    for (uint32_t i = 0; ok && i < groupLen; i++) {
      RunRecord rec{};
      ok = readRunRecordAt(groupStart + i, rec);
      if (ok) segment[i] = rec.blobOffset;
    }
    ok = ok && sortSegmentByName(groupLen);
    for (uint32_t i = 0; ok && i < groupLen; i++) {
      ok = appendOffset(segment[i]);
    }
    return ok;
  };

  auto appendLargeTieGroup = [&](uint32_t groupStart, uint32_t groupLen) {
    Storage.remove(bs.tiePathA);
    Storage.remove(bs.tiePathB);

    {
      auto out = Storage.open(bs.tiePathA, O_RDWR | O_CREAT | O_TRUNC);
      if (!out) {
        LOG_ERR("FIDX", "cannot open tie scratch");
        return false;
      }

      for (uint32_t base = 0; ok && base < groupLen; base += MAX_TIE_SEGMENT) {
        const uint32_t len = std::min<uint32_t>(MAX_TIE_SEGMENT, groupLen - base);
        for (uint32_t i = 0; ok && i < len; i++) {
          RunRecord rec{};
          ok = readRunRecordAt(groupStart + base + i, rec);
          if (ok) segment[i] = rec.blobOffset;
        }
        ok = ok && sortSegmentByName(len);
        if (ok) ok = out.write(segment, len * sizeof(uint32_t)) == len * sizeof(uint32_t);
        maybeYield(bs.yieldCounter);
      }
      out.flush();
      out.close();
      if (!ok) return false;
    }

    const char* inPath = bs.tiePathA;
    const char* outPath = bs.tiePathB;
    for (uint32_t runLen = MAX_TIE_SEGMENT; ok && runLen < groupLen; runLen *= 2) {
      auto in = Storage.open(inPath);
      auto out = Storage.open(outPath, O_RDWR | O_CREAT | O_TRUNC);
      if (!in || !out) {
        LOG_ERR("FIDX", "cannot open tie merge scratch");
        if (in) in.close();
        if (out) out.close();
        return false;
      }

      for (uint32_t base = 0; ok && base < groupLen; base += runLen * 2) {
        const uint32_t lenA = std::min<uint32_t>(runLen, groupLen - base);
        const uint32_t lenB = std::min<uint32_t>(runLen, groupLen - base - lenA);
        uint32_t ia = 0;
        uint32_t ib = 0;
        bool haveA = false;
        bool haveB = false;
        uint32_t offsetA = 0;
        uint32_t offsetB = 0;

        while (ok && (ia < lenA || ib < lenB)) {
          if (!haveA && ia < lenA) {
            ok = readTieOffsetAt(in, base + ia, offsetA);
            haveA = ok;
          }
          if (ok && !haveB && ib < lenB) {
            ok = readTieOffsetAt(in, base + lenA + ib, offsetB);
            haveB = ok;
          }
          if (!ok) break;

          bool takeA = false;
          if (haveA && !haveB) {
            takeA = true;
          } else if (haveA && haveB) {
            takeA = offsetComesBefore(offsetA, offsetB);
            if (!ok) break;
          }
          const uint32_t chosen = takeA ? offsetA : offsetB;
          ok = out.write(&chosen, sizeof(chosen)) == sizeof(chosen);
          if (takeA) {
            haveA = false;
            ia++;
          } else {
            haveB = false;
            ib++;
          }
          maybeYield(bs.yieldCounter);
        }
      }

      in.close();
      out.flush();
      out.close();
      std::swap(inPath, outPath);
    }

    auto sorted = Storage.open(inPath);
    if (!sorted) {
      LOG_ERR("FIDX", "cannot reopen tie scratch");
      return false;
    }
    for (uint32_t i = 0; ok && i < groupLen; i++) {
      uint32_t offset = 0;
      ok = sorted.read(&offset, sizeof(offset)) == static_cast<int>(sizeof(offset)) && appendOffset(offset);
      maybeYield(bs.yieldCounter);
    }
    sorted.close();
    Storage.remove(bs.tiePathA);
    Storage.remove(bs.tiePathB);
    return ok;
  };

  for (uint32_t groupStart = 0; ok && groupStart < recordCount;) {
    RunRecord first{};
    ok = readRunRecordAt(groupStart, first);
    if (!ok) break;

    uint32_t groupLen = 1;
    while (ok && groupStart + groupLen < recordCount) {
      RunRecord next{};
      ok = readRunRecordAt(groupStart + groupLen, next);
      if (!ok || memcmp(first.key, next.key, sizeof(first.key)) != 0) break;
      groupLen++;
      maybeYield(bs.yieldCounter);
    }

    ok = groupLen <= MAX_TIE_SEGMENT ? appendSmallTieGroup(groupStart, groupLen)
                                     : appendLargeTieGroup(groupStart, groupLen);
    groupStart += groupLen;
  }

  if (run) run.close();
  Storage.remove(bs.tiePathA);
  Storage.remove(bs.tiePathB);
  return ok;
}

bool FileIndex::readOffsetForPhysIndex(size_t physIndex, uint32_t& recordOffset) {
  if (offsetsCacheFirst == SIZE_MAX || physIndex < offsetsCacheFirst ||
      physIndex >= offsetsCacheFirst + OFFSETS_CACHE_ENTRIES) {
    const size_t total = totalCount();
    const size_t first = physIndex - (physIndex % OFFSETS_CACHE_ENTRIES);
    const size_t count = std::min(OFFSETS_CACHE_ENTRIES, total - first);
    if (!idxFile.seek(hdr.offsetsStart + first * sizeof(uint32_t)) ||
        idxFile.read(offsetsCache, count * sizeof(uint32_t)) != static_cast<int>(count * sizeof(uint32_t))) {
      offsetsCacheFirst = SIZE_MAX;
      return false;
    }
    offsetsCacheFirst = first;
  }
  recordOffset = offsetsCache[physIndex - offsetsCacheFirst];
  return true;
}

bool FileIndex::entryAt(size_t row, Entry& out) {
  if (!opened || row >= totalCount()) return false;

  uint32_t recordOffset = 0;
  if (!readOffsetForPhysIndex(row, recordOffset)) return false;

  RecordHeader rec{};
  if (!idxFile.seek(recordOffset) || idxFile.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return false;

  const size_t n = std::min<size_t>(rec.nameLen, MAX_NAME);
  if (idxFile.read(out.name, n) != static_cast<int>(n)) return false;
  out.name[n] = '\0';
  out.isDir = (rec.flags & 1) != 0;
  return true;
}

size_t FileIndex::findRowByName(const char* name) {
  if (!opened) return SIZE_MAX;

  uint32_t target = 0;
  bool found = false;
  uint32_t pos = hdr.blobStart;
  const uint32_t blobEnd = hdr.blobStart + hdr.blobLen;
  uint32_t yieldCounter = 0;

  if (!idxFile.seek(pos)) return SIZE_MAX;
  while (pos < blobEnd) {
    RecordHeader rec{};
    if (idxFile.read(&rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) return SIZE_MAX;
    const size_t n = std::min<size_t>(rec.nameLen, MAX_NAME);
    if (idxFile.read(nameBuf.get(), n) != static_cast<int>(n)) return SIZE_MAX;
    nameBuf[n] = '\0';
    if (rec.nameLen <= MAX_NAME && strcmp(nameBuf.get(), name) == 0) {
      target = pos;
      found = true;
      break;
    }
    pos += sizeof(rec) + rec.nameLen;
    if (rec.nameLen > MAX_NAME && !idxFile.seek(pos)) return SIZE_MAX;
    maybeYield(yieldCounter);
  }
  if (!found) return SIZE_MAX;

  const size_t total = totalCount();
  if (!idxFile.seek(hdr.offsetsStart)) return SIZE_MAX;
  for (size_t row = 0; row < total; row++) {
    uint32_t off = 0;
    if (idxFile.read(&off, sizeof(off)) != static_cast<int>(sizeof(off))) return SIZE_MAX;
    if (off == target) return row;
    maybeYield(yieldCounter);
  }
  return SIZE_MAX;
}
