#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "TextPool.h"

// Static member definitions
char Dictionary::wordBuf[256] = "";

namespace {
constexpr char DICT_BIN[] = "dictionary.bin";
std::string lookupDictPathOverride;
bool lookupDictPathOverrideActive = false;
constexpr char GLOBAL_DICT_DIR[] = "/.crosspoint";

// Query-only fallback adapted from CrossPoint Reader PR #4396. Never fold
// index entries/comparators: removing marks changes their lexical order.
bool isLookupVowelMark(const uint32_t cp) {
  const bool hebrew = (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF || (cp >= 0x05C1 && cp <= 0x05C2) ||
                      (cp >= 0x05C4 && cp <= 0x05C5) || (cp >= 0x05C7 && cp <= 0x05C9) || cp == 0xFB1E;
  // Keep Arabic shaddah, hamza and maddah significant.
  return hebrew || (cp >= 0x064B && cp <= 0x0650) || cp == 0x0652 || cp == 0x0670;
}

std::string strippedLookupFallback(const std::string& word) {
  const auto* begin = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* cursor = begin;
  // No allocation at all for queries without removable marks. On a miss this
  // scratch string holds at most word.size() bytes; existing cleanup/NFC may
  // allocate another result. Runtime-sized phrases are unsuitable for stack storage.
  std::string stripped;
  bool changed = false;
  while (*cursor) {
    const auto* start = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (isLookupVowelMark(cp)) {
      if (!changed) {
        stripped.reserve(word.size());
        stripped.assign(word.data(), static_cast<size_t>(start - begin));
        changed = true;
      }
    } else if (changed) {
      stripped.append(reinterpret_cast<const char*>(start), static_cast<size_t>(cursor - start));
    }
  }
  // Marks can hide edge punctuation; trim again only after stripping. Retain
  // CrossInk's case-preserving cleanup and supported NFC composition.
  return changed ? utf8CleanLookupWord(stripped) : std::string{};
}

bool isTextDefinitionType(char type) {
  switch (type) {
    case 'm':  // plain meaning
    case 'l':  // locale-specific plain text
    case 'g':  // Pango markup
    case 't':  // English phonetic text
    case 'x':  // XDXF XML
    case 'y':  // Chinese phonetic text
    case 'k':  // KingSoft PowerWord text
    case 'w':  // MediaWiki text
    case 'h':  // HTML
    case 'n':  // WordNet text
      return true;
    default:
      return false;
  }
}

bool isHtmlDefinitionType(char type) { return type == 'h' || type == 'g' || type == 'x'; }

uint8_t idxEntrySuffixBytes(const DictInfo& info) { return info.idxoffsetbits == 64 ? 12 : 8; }

uint32_t readBigEndian32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t readTextFieldSize(HalFile& file, uint32_t offset, uint32_t maxSize) {
  file.seekSet(offset);
  uint32_t size = 0;
  while (size < maxSize) {
    const int b = file.read();
    if (b < 0 || b == 0) break;
    size++;
  }
  return size;
}

bool skipResourceField(HalFile& file, uint32_t* pos, uint32_t end) {
  if (*pos + 4 > end) return false;
  file.seekSet(*pos);
  uint8_t sizeBuf[4];
  if (file.read(sizeBuf, 4) != 4) return false;
  const uint32_t size = readBigEndian32(sizeBuf);
  if (size > end - *pos - 4) return false;
  *pos += 4 + size;
  return true;
}
}  // namespace

// OFT file constants (StarDict Cache format, verified against real files).
// Header: 30-byte text + 8-byte fixed magic = 38 bytes total.
// Each entry: LE uint32 = byte offset of word (K+1)*32 in the source file.
static constexpr uint32_t OFT_HEADER_SIZE = 38;
static constexpr uint32_t OFT_STRIDE = 32;  // words per page

// .idx.oft.cspt file constants (CrossPoint optimized index).
// Header: 12 bytes = magic(4) + version(1) + prefixLen(1) + stride(2) + entryCount(4).
// Each entry: prefixLen bytes (null-padded headword) + 4-byte LE idx offset = 20 bytes.
static constexpr uint8_t CSPT_MAGIC[4] = {'C', 'S', 'P', 'T'};
static constexpr uint8_t CSPT_VERSION = 1;
static constexpr uint8_t CSPT_PREFIX_LEN = 16;
static constexpr uint16_t CSPT_STRIDE = 16;
static constexpr uint32_t CSPT_HEADER_SIZE = 12;
static constexpr uint32_t CSPT_ENTRY_SIZE = CSPT_PREFIX_LEN + 4;  // 20 bytes

// Device-generated sampled index. The sidecar stores the byte offset of every
// 256th .idx entry, allowing lookup to binary-search a tiny file before scanning
// one bounded window from the original index.
static constexpr uint32_t QIDX_MAGIC = 0x58444951;  // "QIDX" little-endian
static constexpr uint32_t QIDX_VERSION = 1;
static constexpr uint32_t QIDX_SAMPLE_INTERVAL = 256;
static constexpr uint32_t QIDX_HEADER_SIZE = 5 * sizeof(uint32_t);

struct QidxHeader {
  uint32_t sampleCount = 0;
  bool valid = false;
};

static QidxHeader readQidxHeader(HalFile& qidx, uint32_t idxFileSize) {
  QidxHeader result;
  uint32_t raw[5];
  if (!qidx.seekSet(0) || qidx.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return result;
  if (raw[0] != QIDX_MAGIC || raw[1] != QIDX_VERSION || raw[2] != QIDX_SAMPLE_INTERVAL || raw[3] == 0 ||
      raw[4] != idxFileSize) {
    return result;
  }
  if (raw[3] > (UINT32_MAX - QIDX_HEADER_SIZE) / sizeof(uint32_t)) return result;
  const uint32_t requiredSize = QIDX_HEADER_SIZE + raw[3] * sizeof(uint32_t);
  if (qidx.fileSize() < requiredSize) return result;
  result.sampleCount = raw[3];
  result.valid = true;
  return result;
}

static bool readQidxOffset(HalFile& qidx, uint32_t sampleIndex, uint32_t* offset) {
  return qidx.seekSet(QIDX_HEADER_SIZE + sampleIndex * sizeof(uint32_t)) &&
         qidx.read(offset, sizeof(*offset)) == static_cast<int>(sizeof(*offset));
}

// ---------------------------------------------------------------------------
// Path management
// ---------------------------------------------------------------------------

std::string Dictionary::readDictPath(const char* cachePath) {
  if (lookupDictPathOverrideActive) return lookupDictPathOverride;
  return readConfiguredDictPath(cachePath);
}

std::string Dictionary::readConfiguredDictPath(const char* cachePath) {
  char binPath[128];

  // Try per-book dictionary.bin first when cachePath is provided.
  if (cachePath && cachePath[0] != '\0') {
    snprintf(binPath, sizeof(binPath), "%s/%s", cachePath, DICT_BIN);
    HalFile f;
    if (Storage.openFileForRead("DICT", binPath, f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
        std::string result(sz, '\0');
        const int n = f.read(&result[0], sz);
        f.close();
        if (n > 0) {
          result.resize(static_cast<size_t>(n));
          return result;
        }
      } else {
        f.close();
      }
    }
    // Per-book file absent or empty ("Use Global") — fall through to global.
  }

  // Read global dictionary.bin.
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForRead("DICT", binPath, f)) return "";
  const int sz = static_cast<int>(f.fileSize());
  if (sz <= 0) {
    f.close();
    return "";
  }
  std::string result(sz, '\0');
  const int n = f.read(&result[0], sz);
  f.close();
  if (n <= 0) return "";
  result.resize(static_cast<size_t>(n));
  return result;
}

void Dictionary::setLookupDictPathOverride(const char* folderPath) {
  lookupDictPathOverride = folderPath ? folderPath : "";
  lookupDictPathOverrideActive = true;
}

void Dictionary::clearLookupDictPathOverride() {
  lookupDictPathOverrideActive = false;
  std::string().swap(lookupDictPathOverride);
}

void Dictionary::saveGlobalDictPath(const char* folderPath) {
  char binPath[128];
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForWrite("DICT", binPath, f)) {
    LOG_ERR("DICT", "Could not write global dictionary path");
    return;
  }
  if (folderPath && folderPath[0] != '\0') {
    f.write(reinterpret_cast<const uint8_t*>(folderPath), strlen(folderPath));
  }
  f.close();
}

bool Dictionary::hasGlobalDictPathFile() {
  char binPath[128];
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  return Storage.exists(binPath);
}

// ---------------------------------------------------------------------------
// Validity checks
// ---------------------------------------------------------------------------

bool Dictionary::exists(const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  if (!Storage.exists(dp.idx().c_str())) return false;
  return Storage.exists(dp.dict().c_str());
}

bool Dictionary::hasAltForms(const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return false;
  return Storage.exists(DictPaths(folderPath).syn().c_str());
}

bool Dictionary::isValidDictionary() {
  std::string folderPath = readDictPath(nullptr);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  const bool idxExists = Storage.exists(dp.idx().c_str());
  const bool valid = idxExists && Storage.exists(dp.dict().c_str());
  if (!valid) {
    LOG_DBG("DICT", "Stored dictionary path no longer valid, resetting");
    saveGlobalDictPath("");
  }
  return valid;
}

// ---------------------------------------------------------------------------
// .ifo parsing
// ---------------------------------------------------------------------------

DictInfo Dictionary::readInfo(const char* folderPath) {
  DictInfo info;
  if (folderPath == nullptr || folderPath[0] == '\0') return info;

  std::string folder(folderPath);
  std::string ifoPath = DictPaths(folder).ifo();

  HalFile file;
  if (!Storage.openFileForRead("DICT", ifoPath.c_str(), file)) return info;

  // Validate header line byte by byte — no line buffer needed.
  static constexpr const char HEADER[] = "StarDict's dict ifo file";
  for (size_t i = 0; i < sizeof(HEADER) - 1; i++) {
    int b = file.read();
    if (b < 0 || static_cast<char>(b) != HEADER[i]) {
      LOG_ERR("DICT", "Invalid .ifo header in %s", folderPath);
      file.close();
      return info;
    }
  }
  // Skip remainder of header line.
  {
    int b;
    while ((b = file.read()) >= 0 && b != '\n') {
    }
  }

  // Serial key=value parse. State fits in ~50 bytes vs the old 512-byte slurp buffer.
  char keyBuf[24];  // longest key: "sametypesequence" = 16 chars
  int keyLen = 0;
  bool readingVal = false;
  char* valDst = nullptr;
  size_t valCap = 0;
  size_t valWritten = 0;
  bool isNumField = false;
  uint32_t* valNum = nullptr;
  uint32_t numAccum = 0;

  while (file.available()) {
    int b = file.read();
    if (b < 0) break;
    const char c = static_cast<char>(b);

    if (c == '\r') continue;

    if (!readingVal) {
      if (c == '\n') {
        keyLen = 0;
      } else if (c == '=') {
        keyBuf[keyLen] = '\0';
        keyLen = 0;
        valDst = nullptr;
        valCap = 0;
        valWritten = 0;
        isNumField = false;
        valNum = nullptr;
        numAccum = 0;
        if (strcmp(keyBuf, "bookname") == 0) {
          valDst = info.bookname;
          valCap = sizeof(info.bookname) - 1;
        } else if (strcmp(keyBuf, "sametypesequence") == 0) {
          valDst = info.sametypesequence;
          valCap = sizeof(info.sametypesequence) - 1;
        } else if (strcmp(keyBuf, "website") == 0) {
          valDst = info.website;
          valCap = sizeof(info.website) - 1;
        } else if (strcmp(keyBuf, "date") == 0) {
          valDst = info.date;
          valCap = sizeof(info.date) - 1;
        } else if (strcmp(keyBuf, "description") == 0) {
          valDst = info.description;
          valCap = sizeof(info.description) - 1;
        } else if (strcmp(keyBuf, "lang") == 0) {
          valDst = info.lang;
          valCap = sizeof(info.lang) - 1;
        } else if (strcmp(keyBuf, "wordcount") == 0) {
          isNumField = true;
          valNum = &info.wordcount;
        } else if (strcmp(keyBuf, "idxfilesize") == 0) {
          isNumField = true;
          valNum = &info.idxfilesize;
        } else if (strcmp(keyBuf, "idxoffsetbits") == 0) {
          isNumField = true;
          valNum = &info.idxoffsetbits;
        } else if (strcmp(keyBuf, "synwordcount") == 0) {
          isNumField = true;
          valNum = &info.altFormCount;
          info.hasAltForms = true;
        }
        readingVal = true;
      } else if (keyLen < static_cast<int>(sizeof(keyBuf) - 1)) {
        keyBuf[keyLen++] = c;
      }
    } else {
      if (c == '\n') {
        if (valDst) valDst[valWritten] = '\0';
        if (isNumField && valNum) *valNum = numAccum;
        readingVal = false;
        keyLen = 0;
      } else if (isNumField) {
        if (c >= '0' && c <= '9') numAccum = numAccum * 10 + static_cast<uint32_t>(c - '0');
      } else if (valDst && valWritten < valCap) {
        valDst[valWritten++] = c;
      }
    }
  }

  file.close();

  DictPaths dp(folder);
  const bool dictExists = Storage.exists(dp.dict().c_str());
  info.isCompressed = !dictExists && Storage.exists(dp.dictDz().c_str());
  if (info.idxoffsetbits != 64) info.idxoffsetbits = 32;

  info.valid = true;
  return info;
}

// ---------------------------------------------------------------------------
// Word cleaning
// ---------------------------------------------------------------------------

std::string Dictionary::cleanWord(const std::string& word) { return utf8CleanLookupWord(word); }

// ---------------------------------------------------------------------------
// Low-level file reading helpers
// ---------------------------------------------------------------------------

int Dictionary::readWordInto(HalFile& file, char* buf, size_t bufSize) {
  size_t i = 0;
  while (i < bufSize - 1) {
    int ch = file.read();
    if (ch < 0) return -1;  // EOF or I/O error
    if (ch == 0) {
      buf[i] = '\0';
      return static_cast<int>(i);
    }
    buf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync
  buf[bufSize - 1] = '\0';
  int ch;
  do {
    ch = file.read();
  } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

// ---------------------------------------------------------------------------
// OFT binary search helper
// ---------------------------------------------------------------------------

// Case-insensitive strcmp for ASCII — used in findPageBounds() because StarDict
// dictionaries (including wiktionary-derived ones) are sorted case-insensitively.
// Using plain strcmp would cause the binary search to land on the wrong page for
// any word whose alphabetic neighbourhood contains mixed-case page boundaries.
static int cistrcmp(const char* a, const char* b) {
  while (*a && *b) {
    int diff = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    a++;
    b++;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

// CLEANUP: on Auto-only commit, delete only this line (readCsptEntryCount below stays)
uint32_t Dictionary::readCsptEntryCount(const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return 0;
  HalFile cspt;
  if (!Storage.openFileForRead("DICT", DictPaths(folderPath).idxOftCspt().c_str(), cspt)) return 0;
  uint8_t hdr[CSPT_HEADER_SIZE];
  cspt.seekSet(0);
  const bool read_ok = (cspt.read(hdr, CSPT_HEADER_SIZE) == static_cast<int>(CSPT_HEADER_SIZE));
  cspt.close();
  if (!read_ok) return 0;
  if (memcmp(hdr, CSPT_MAGIC, 4) != 0 || hdr[4] != CSPT_VERSION) return 0;
  uint32_t entryCount;
  memcpy(&entryCount, hdr + 8, 4);  // LE, matches binarySearchCspt
  return entryCount;
}

bool Dictionary::buildQuickIndex(HalFile& idx, uint32_t idxFileSize, uint8_t suffixBytes, const char* qidxPath,
                                 const DictLookupCallbacks& cbs) {
  constexpr size_t SCAN_BUFFER_SIZE = 1024;
  // The lookup task stack is only 4KB and this is a cold, fallible operation, so
  // use one transient 1KB heap buffer instead of retaining DRAM or growing vectors.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(SCAN_BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("DICT", "OOM: %u byte quick-index buffer", static_cast<unsigned>(SCAN_BUFFER_SIZE));
    return false;
  }

  const std::string tmpPath = std::string(qidxPath) + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("DICT", tmpPath, out)) return false;

  const uint32_t placeholder[5] = {};
  uint32_t sampleCount = 1;
  const uint32_t firstOffset = 0;
  bool ok = out.write(placeholder, sizeof(placeholder)) == static_cast<int>(sizeof(placeholder)) &&
            out.write(&firstOffset, sizeof(firstOffset)) == static_cast<int>(sizeof(firstOffset)) && idx.seekSet(0);
  uint32_t entryCount = 0;
  uint32_t position = 0;
  uint8_t suffixLeft = 0;

  while (ok && position < idxFileSize) {
    if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) {
      ok = false;
      break;
    }
    const size_t wanted = std::min<size_t>(SCAN_BUFFER_SIZE, idxFileSize - position);
    const int bytesRead = idx.read(buffer.get(), wanted);
    if (bytesRead <= 0) {
      LOG_ERR("DICT", "Quick-index read failed at %lu", static_cast<unsigned long>(position));
      ok = false;
      break;
    }
    for (int i = 0; ok && i < bytesRead; i++) {
      if (suffixLeft == 0) {
        if (buffer[i] == 0) suffixLeft = suffixBytes;
      } else if (--suffixLeft == 0) {
        entryCount++;
        const uint32_t nextEntryOffset = position + static_cast<uint32_t>(i) + 1;
        if (entryCount % QIDX_SAMPLE_INTERVAL == 0 && nextEntryOffset < idxFileSize) {
          ok = out.write(&nextEntryOffset, sizeof(nextEntryOffset)) == static_cast<int>(sizeof(nextEntryOffset));
          if (ok) sampleCount++;
        }
      }
    }
    position += static_cast<uint32_t>(bytesRead);
    if (cbs.onProgress && idxFileSize > 0) {
      cbs.onProgress(cbs.ctx, static_cast<int>((static_cast<uint64_t>(position) * 65) / idxFileSize));
    }
  }

  if (ok && suffixLeft != 0) {
    LOG_ERR("DICT", "Quick-index scan found a truncated .idx entry");
    ok = false;
  }
  if (ok) {
    const uint32_t header[5] = {QIDX_MAGIC, QIDX_VERSION, QIDX_SAMPLE_INTERVAL, sampleCount, idxFileSize};
    ok = out.seekSet(0) && out.write(header, sizeof(header)) == static_cast<int>(sizeof(header));
  }
  out.close();

  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(qidxPath) && !Storage.remove(qidxPath)) {
    LOG_ERR("DICT", "Could not replace stale quick index %s", qidxPath);
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), qidxPath)) {
    LOG_ERR("DICT", "Could not install quick index %s", qidxPath);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  LOG_INF("DICT", "Indexed %lu entries into %lu quick samples", static_cast<unsigned long>(entryCount),
          static_cast<unsigned long>(sampleCount));
  return true;
}

bool Dictionary::prepareQuickIndex(const DictLookupCallbacks& cbs, const char* cachePath) {
  const std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return false;
  const DictPaths paths(folderPath);

  // Desktop-generated accelerators are denser and remain the preferred path.
  if (Storage.exists(paths.idxOftCspt().c_str()) || Storage.exists(paths.idxOft().c_str())) return true;

  HalFile idx;
  if (!Storage.openFileForRead("DICT", paths.idx().c_str(), idx)) return false;
  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());

  HalFile qidx;
  if (Storage.openFileForRead("DICT", paths.quickIdx().c_str(), qidx)) {
    const bool valid = readQidxHeader(qidx, idxFileSize).valid;
    qidx.close();
    if (valid) {
      idx.close();
      return true;
    }
  }

  const uint8_t suffixBytes = idxEntrySuffixBytes(readInfo(folderPath.c_str()));
  const bool built = buildQuickIndex(idx, idxFileSize, suffixBytes, paths.quickIdx().c_str(), cbs);
  idx.close();
  return built;
}

bool Dictionary::binarySearchCspt(HalFile& cspt, const char* target, uint32_t idxFileSize, uint32_t* startByte,
                                  uint32_t* endByte, bool startBeforeCaseMatches) {
  // Read and validate header (12 bytes).
  uint8_t hdr[CSPT_HEADER_SIZE];
  cspt.seekSet(0);
  if (cspt.read(hdr, CSPT_HEADER_SIZE) != static_cast<int>(CSPT_HEADER_SIZE)) return false;
  if (memcmp(hdr, CSPT_MAGIC, 4) != 0 || hdr[4] != CSPT_VERSION) return false;

  const uint8_t prefixLen = hdr[5];
  uint16_t stride;
  memcpy(&stride, hdr + 6, 2);  // LE
  uint32_t entryCount;
  memcpy(&entryCount, hdr + 8, 4);  // LE

  if (entryCount == 0 || prefixLen == 0 || prefixLen > 128) {
    *startByte = 0;
    *endByte = idxFileSize;
    return true;
  }

  const uint32_t entrySize = prefixLen + 4;

  // Binary search for the last usable sample. Exact-case lookup starts before
  // the first case-insensitive match so every capitalization variant is scanned.
  uint32_t lo = 0, hi = entryCount - 1;
  uint8_t entry[128 + 4];  // prefixLen capped at 128 above

  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    cspt.seekSet(CSPT_HEADER_SIZE + mid * entrySize);
    if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;

    // Null-terminate prefix for cistrcmp (prefix is already null-padded if shorter).
    entry[prefixLen] = '\0';
    const int cmp = cistrcmp(reinterpret_cast<const char*>(entry), target);
    if (cmp > 0 || (startBeforeCaseMatches && cmp == 0)) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  // Read the matched entry to get idxOffset.
  cspt.seekSet(CSPT_HEADER_SIZE + lo * entrySize);
  if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;

  uint32_t idxOffset;
  memcpy(&idxOffset, entry + prefixLen, 4);  // LE
  *startByte = idxOffset;

  // End bound: read next entry's idxOffset, or use idxFileSize for last entry.
  if (lo + 1 < entryCount) {
    cspt.seekSet(CSPT_HEADER_SIZE + (lo + 1) * entrySize);
    if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;
    memcpy(endByte, entry + prefixLen, 4);  // LE
  } else {
    *endByte = idxFileSize;
  }

  return true;
}

bool Dictionary::binarySearchQuickIndex(HalFile& qidx, HalFile& idx, const uint32_t sampleCount, const char* target,
                                        const uint32_t idxFileSize, uint32_t* startByte, uint32_t* endByte,
                                        uint32_t* matchedSample) {
  if (sampleCount == 0) return false;
  uint32_t lo = 0;
  uint32_t hi = sampleCount - 1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo + 1) / 2;
    uint32_t offset = 0;
    if (!readQidxOffset(qidx, mid, &offset) || offset >= idxFileSize || !idx.seekSet(offset) ||
        readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) {
      return false;
    }
    if (cistrcmp(wordBuf, target) < 0) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  if (!readQidxOffset(qidx, lo, startByte) || *startByte >= idxFileSize) return false;
  if (lo + 1 < sampleCount) {
    if (!readQidxOffset(qidx, lo + 1, endByte) || *endByte <= *startByte || *endByte > idxFileSize) return false;
  } else {
    *endByte = idxFileSize;
  }
  if (matchedSample) *matchedSample = lo;
  return true;
}

bool Dictionary::resolveScanBounds(const char* csptPath, const char* oftPath, HalFile& src, uint32_t srcFileSize,
                                   const char* target, uint32_t* startByte, uint32_t* endByte,
                                   bool startBeforeCaseMatches) {
  // Try .cspt first (CrossPoint optimized index), fall back to .oft.
  bool boundsResolved = false;
  HalFile cspt;
  if (Storage.openFileForRead("DICT", csptPath, cspt)) {
    boundsResolved = binarySearchCspt(cspt, target, srcFileSize, startByte, endByte, startBeforeCaseMatches);
    cspt.close();
  }
  if (!boundsResolved) {
    HalFile oft;
    if (Storage.openFileForRead("DICT", oftPath, oft)) {
      findPageBounds(oft, src, srcFileSize, target, startByte, endByte, startBeforeCaseMatches);
      boundsResolved = true;
      oft.close();
    }
  }
  return boundsResolved;
}

void Dictionary::findPageBounds(HalFile& oft, HalFile& src, uint32_t srcFileSize, const char* target,
                                uint32_t* startByte, uint32_t* endByte, bool startBeforeCaseMatches) {
  const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
  const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;
  const uint32_t numPages = numEntries + 1;  // page 0 is implicit (starts at byte 0 in src)

  if (numEntries == 0) {
    // No OFT entries — entire source file is one page
    *startByte = 0;
    *endByte = srcFileSize;
    return;
  }

  // Returns the byte offset in src where page K begins.
  // Page 0 always starts at 0; page K>0 is stored in OFT entry K-1 (LE uint32).
  auto pageStart = [&](uint32_t k) -> uint32_t {
    if (k == 0) return 0;
    oft.seekSet(OFT_HEADER_SIZE + (k - 1) * 4);
    uint8_t raw[4];
    if (oft.read(raw, 4) != 4) return srcFileSize;
    uint32_t val;
    memcpy(&val, raw, 4);  // little-endian on ESP32-C3
    return val;
  };

  // Binary search: find the last page whose first word <= target
  uint32_t lo = 0, hi = numPages - 1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    uint32_t midStart = pageStart(mid);
    src.seekSet(midStart);
    int len = readWordInto(src, wordBuf, sizeof(wordBuf));
    const int cmp = len < 0 ? 1 : cistrcmp(wordBuf, target);
    if (cmp > 0 || (startBeforeCaseMatches && cmp == 0)) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  *startByte = pageStart(lo);
  *endByte = (lo + 1 < numPages) ? pageStart(lo + 1) : srcFileSize;
}

// ---------------------------------------------------------------------------
// Reading helpers
// ---------------------------------------------------------------------------

std::string Dictionary::readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size) {
  HalFile dict;
  if (!Storage.openFileForRead("DICT", DictPaths(folderPath).dict().c_str(), dict)) return "";

  dict.seekSet(offset);

  std::string def(size, '\0');
  int bytesRead = dict.read(reinterpret_cast<uint8_t*>(&def[0]), size);
  dict.close();

  if (bytesRead < 0) return "";
  if (static_cast<uint32_t>(bytesRead) < size) def.resize(bytesRead);
  return def;
}

DictDefinitionSlice Dictionary::resolveDefinitionSlice(const DictLocation& location, const DictInfo& info) {
  DictDefinitionSlice slice;
  slice.folderPath = location.folderPath;
  if (!location.found || location.folderPath.empty() || location.size == 0) return slice;

  const uint32_t entryEnd = location.offset + location.size;
  if (entryEnd < location.offset) return slice;

  HalFile dict;
  if (!Storage.openFileForRead("DICT", DictPaths(location.folderPath).dict().c_str(), dict)) return slice;

  auto setSlice = [&](uint32_t offset, uint32_t size, char type) {
    slice.offset = offset;
    slice.size = size;
    slice.type = type;
    slice.isHtml = isHtmlDefinitionType(type);
    slice.found = true;
  };

  if (info.valid && info.sametypesequence[0] != '\0') {
    uint32_t pos = location.offset;
    for (size_t i = 0; info.sametypesequence[i] != '\0' && pos < entryEnd; i++) {
      const char type = info.sametypesequence[i];
      if (isTextDefinitionType(type)) {
        uint32_t size = readTextFieldSize(dict, pos, entryEnd - pos);
        if (size == 0 && pos < entryEnd) size = entryEnd - pos;
        setSlice(pos, size, type);
        dict.close();
        return slice;
      }
      if ((type == 'W' || type == 'P') && skipResourceField(dict, &pos, entryEnd)) continue;
      break;
    }
  } else if (info.valid) {
    uint32_t pos = location.offset;
    while (pos < entryEnd) {
      dict.seekSet(pos);
      const int typeRaw = dict.read();
      if (typeRaw < 0) break;
      const char type = static_cast<char>(typeRaw);
      pos++;

      if (isTextDefinitionType(type)) {
        const uint32_t size = readTextFieldSize(dict, pos, entryEnd - pos);
        setSlice(pos, size, type);
        dict.close();
        return slice;
      }
      if ((type == 'W' || type == 'P') && skipResourceField(dict, &pos, entryEnd)) continue;
      break;
    }
  }

  // Fallback for dictionaries without usable .ifo metadata: preserve the old
  // behavior and render the raw range as plain text.
  setSlice(location.offset, location.size, 'm');
  dict.close();
  return slice;
}

// ---------------------------------------------------------------------------
// Locate (index search only — no definition read, zero RAM growth)
// ---------------------------------------------------------------------------

bool Dictionary::openLookupSession(LookupSession& session, const char* cachePath) {
  session.folderPath = readDictPath(cachePath);
  if (session.folderPath.empty()) return false;

  const DictPaths paths(session.folderPath);
  session.suffixBytes = idxEntrySuffixBytes(readInfo(session.folderPath.c_str()));
  const std::string idxPath = paths.idx();
  if (!Storage.openFileForRead("DICT", idxPath.c_str(), session.idx)) {
    LOG_ERR("DICT", "Failed to open index %s", idxPath.c_str());
    return false;
  }
  session.idxFileSize = static_cast<uint32_t>(session.idx.fileSize());

  if (session.suffixBytes == 8) {
    const std::string csptPath = paths.idxOftCspt();
    if (Storage.openFileForRead("DICT", csptPath.c_str(), session.accelerator)) {
      uint8_t header[CSPT_HEADER_SIZE];
      const bool headerRead = session.accelerator.seekSet(0) &&
                              session.accelerator.read(header, sizeof(header)) == static_cast<int>(sizeof(header));
      uint32_t entryCount = 0;
      if (headerRead) memcpy(&entryCount, header + 8, sizeof(entryCount));
      const uint8_t prefixLen = headerRead ? header[5] : 0;
      const uint64_t requiredSize = static_cast<uint64_t>(CSPT_HEADER_SIZE) +
                                    static_cast<uint64_t>(entryCount) * (static_cast<uint32_t>(prefixLen) + 4U);
      if (headerRead && memcmp(header, CSPT_MAGIC, sizeof(CSPT_MAGIC)) == 0 && header[4] == CSPT_VERSION &&
          prefixLen > 0 && prefixLen <= 128 && requiredSize <= session.accelerator.fileSize()) {
        session.acceleratorKind = LookupAccelerator::Cspt;
        return true;
      }
      session.accelerator.close();
    }

    const std::string oftPath = paths.idxOft();
    if (Storage.openFileForRead("DICT", oftPath.c_str(), session.accelerator)) {
      session.acceleratorKind = LookupAccelerator::Oft;
      return true;
    }
  }

  const std::string qidxPath = paths.quickIdx();
  if (Storage.openFileForRead("DICT", qidxPath.c_str(), session.accelerator)) {
    const QidxHeader header = readQidxHeader(session.accelerator, session.idxFileSize);
    if (header.valid && header.sampleCount > 0) {
      session.qidxSampleCount = header.sampleCount;
      session.acceleratorKind = LookupAccelerator::QuickIndex;
    } else {
      session.accelerator.close();
    }
  }
  return true;
}

void Dictionary::closeLookupSession(LookupSession& session) {
  session.accelerator.close();
  session.idx.close();
}

DictLocation Dictionary::locateInSession(LookupSession& session, const std::string& word,
                                         const DictLookupCallbacks& cbs) {
  DictLocation result = locateExactInSession(session, word, cbs);
  if (result.found || result.readError || (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx))) return result;
  const std::string fallback = strippedLookupFallback(word);
  if (!fallback.empty() && fallback != word) result = locateExactInSession(session, fallback, cbs);
  return result;
}

DictLocation Dictionary::locateExactInSession(LookupSession& session, const std::string& word,
                                              const DictLookupCallbacks& cbs) {
  DictLocation result;
  uint32_t startByte = 0;
  uint32_t endByte = session.idxFileSize;

  switch (session.acceleratorKind) {
    case LookupAccelerator::Cspt:
      if (!binarySearchCspt(session.accelerator, word.c_str(), session.idxFileSize, &startByte, &endByte, true)) {
        // A structurally valid but unreadable .cspt should degrade to .oft for
        // this and later probes, matching the single-locate fallback order.
        session.accelerator.close();
        const std::string oftPath = DictPaths(session.folderPath).idxOft();
        if (Storage.openFileForRead("DICT", oftPath.c_str(), session.accelerator)) {
          session.acceleratorKind = LookupAccelerator::Oft;
          findPageBounds(session.accelerator, session.idx, session.idxFileSize, word.c_str(), &startByte, &endByte,
                         true);
        } else {
          session.acceleratorKind = LookupAccelerator::None;
        }
      }
      break;
    case LookupAccelerator::Oft:
      findPageBounds(session.accelerator, session.idx, session.idxFileSize, word.c_str(), &startByte, &endByte, true);
      break;
    case LookupAccelerator::QuickIndex:
      if (!binarySearchQuickIndex(session.accelerator, session.idx, session.qidxSampleCount, word.c_str(),
                                  session.idxFileSize, &startByte, &endByte)) {
        session.accelerator.close();
        session.acceleratorKind = LookupAccelerator::None;
        startByte = 0;
        endByte = session.idxFileSize;
      }
      break;
    case LookupAccelerator::None:
      break;
  }

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 70);

  if (!session.idx.seekSet(startByte)) {
    LOG_ERR("DICT", "Failed to seek index %s", session.folderPath.c_str());
    result.readError = true;
    return result;
  }

  // The start bound deliberately precedes any case-equivalent samples. Scan
  // until lexical order passes the target so a match exactly on the next
  // accelerator boundary is included too.
  while (static_cast<uint32_t>(session.idx.position()) < session.idxFileSize) {
    if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) {
      return result;
    }

    int len = readWordInto(session.idx, wordBuf, sizeof(wordBuf));
    if (len < 0) {
      // The loop already guards normal EOF with position() < fileSize(), so a
      // failed read here means a truncated or unreadable index.
      LOG_ERR("DICT", "Failed reading index entry from %s", session.folderPath.c_str());
      result.readError = true;
      break;
    }

    uint8_t suffix[12];
    if (session.idx.read(suffix, session.suffixBytes) != session.suffixBytes) {
      LOG_ERR("DICT", "Truncated index entry in %s", session.folderPath.c_str());
      result.readError = true;
      break;
    }

    int cmp = cistrcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      if (session.suffixBytes == 12 && readBigEndian32(suffix) != 0) {
        LOG_ERR("DICT", "64-bit dictionary offset exceeds supported range");
        return result;
      }
      const uint8_t sizeOffset = session.suffixBytes == 12 ? 8 : 4;
      const bool exactCase = strcmp(wordBuf, word.c_str()) == 0;
      if (!result.found || exactCase) {
        result.headword.assign(wordBuf, static_cast<size_t>(len));
        result.offset = session.suffixBytes == 12 ? readBigEndian32(suffix + 4) : readBigEndian32(suffix);
        result.size = readBigEndian32(suffix + sizeOffset);
        result.found = true;
      }
      if (exactCase) {
        if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
        return result;
      }
      continue;
    }

    if (cmp > 0) break;
  }

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
  return result;
}

DictLocation Dictionary::locate(const std::string& word, const DictLookupCallbacks& cbs, const char* cachePath) {
  LookupSession session;
  if (!openLookupSession(session, cachePath)) {
    DictLocation result;
    result.readError = !session.folderPath.empty();
    return result;
  }
  DictLocation result = locateInSession(session, word, cbs);
  result.folderPath = session.folderPath;
  closeLookupSession(session);
  return result;
}

DictLocation Dictionary::locateWithStemVariants(const std::string& word, bool* matchedStem,
                                                const DictLookupCallbacks& cbs, const char* cachePath) {
  if (matchedStem) *matchedStem = false;
  LookupSession session;
  if (!openLookupSession(session, cachePath)) {
    DictLocation result;
    result.readError = !session.folderPath.empty();
    return result;
  }

  DictLocation result = locateInSession(session, word, cbs);
  if (!result.found && !result.readError && !(cbs.shouldCancel && cbs.shouldCancel(cbs.ctx))) {
    const auto stems = getStemVariants(word);
    for (const auto& stem : stems) {
      result = locateInSession(session, stem, cbs);
      if (result.found) {
        if (matchedStem) *matchedStem = true;
        break;
      }
      if (result.readError || (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx))) break;
    }
  }

  result.folderPath = session.folderPath;
  closeLookupSession(session);
  return result;
}

// ---------------------------------------------------------------------------
// Lookup (convenience wrapper — locate + read into string)
// ---------------------------------------------------------------------------

std::string Dictionary::lookup(const std::string& word, const DictLookupCallbacks& cbs, const char* cachePath) {
  auto loc = locate(word, cbs, cachePath);
  if (!loc.found) return "";
  const DictInfo info = readInfo(loc.folderPath.c_str());
  const DictDefinitionSlice slice = resolveDefinitionSlice(loc, info);
  if (!slice.found) return "";
  return readDefinition(slice.folderPath, slice.offset, slice.size);
}

// ---------------------------------------------------------------------------
// Alternate-form lookup (.syn)
// ---------------------------------------------------------------------------

// Resolve the word at 0-based ordinal in .idx using .idx.oft for fast page seek.
std::string Dictionary::wordAtOrdinal(const std::string& folderPath, uint32_t ordinal) {
  DictPaths dp(folderPath);
  const DictInfo info = readInfo(folderPath.c_str());
  const uint8_t suffixBytes = idxEntrySuffixBytes(info);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return "";

  const uint32_t pageNum = ordinal / OFT_STRIDE;
  const uint32_t withinPage = ordinal % OFT_STRIDE;
  uint32_t pageStartByte = 0;
  uint32_t entriesToSkip = ordinal;

  if (suffixBytes == 8 && pageNum > 0) {
    HalFile oft;
    if (Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft)) {
      oft.seekSet(OFT_HEADER_SIZE + (pageNum - 1) * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) == 4) memcpy(&pageStartByte, raw, 4);  // LE uint32
      oft.close();
      entriesToSkip = withinPage;
    }
  }

  idx.seekSet(pageStartByte);

  // Skip entries to reach the target. With a usable .idx.oft this is at most
  // 31 entries; otherwise fall back to a full ordinal scan.
  for (uint32_t i = 0; i < entriesToSkip; i++) {
    if (readWordInto(idx, wordBuf, sizeof(wordBuf)) < 0) {
      idx.close();
      return "";
    }
    uint8_t skip[12];
    if (idx.read(skip, suffixBytes) != suffixBytes) {
      idx.close();
      return "";
    }
  }

  int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
  idx.close();
  if (len < 0) return "";
  return std::string(wordBuf, static_cast<size_t>(len));
}

std::string Dictionary::resolveAltForm(const std::string& word, const char* cachePath) {
  std::string headword = resolveExactAltForm(word, cachePath);
  if (!headword.empty()) return headword;
  const std::string fallback = strippedLookupFallback(word);
  return !fallback.empty() && fallback != word ? resolveExactAltForm(fallback, cachePath) : std::string{};
}

std::string Dictionary::resolveExactAltForm(const std::string& word, const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return "";

  DictPaths dp(folderPath);
  if (!Storage.exists(dp.syn().c_str())) return "";

  HalFile syn;
  if (!Storage.openFileForRead("DICT", dp.syn().c_str(), syn)) return "";

  const uint32_t synFileSize = static_cast<uint32_t>(syn.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = synFileSize;

  resolveScanBounds(dp.synOftCspt().c_str(), dp.synOft().c_str(), syn, synFileSize, word.c_str(), &startByte, &endByte,
                    true);

  syn.seekSet(startByte);

  bool fallbackFound = false;
  uint32_t fallbackIdx = 0;
  // As with .idx lookup, include a case-equivalent entry that begins exactly
  // on the next accelerator boundary.
  while (static_cast<uint32_t>(syn.position()) < synFileSize) {
    int len = readWordInto(syn, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t idxBuf[4];
    if (syn.read(idxBuf, 4) != 4) break;

    int cmp = cistrcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      // Big-endian original word index in .idx
      uint32_t originalIdx = (static_cast<uint32_t>(idxBuf[0]) << 24) | (static_cast<uint32_t>(idxBuf[1]) << 16) |
                             (static_cast<uint32_t>(idxBuf[2]) << 8) | static_cast<uint32_t>(idxBuf[3]);
      if (strcmp(wordBuf, word.c_str()) == 0) {
        syn.close();
        return wordAtOrdinal(folderPath, originalIdx);
      }
      if (!fallbackFound) {
        fallbackFound = true;
        fallbackIdx = originalIdx;
      }
      continue;
    }

    if (cmp > 0) break;
  }

  syn.close();
  return fallbackFound ? wordAtOrdinal(folderPath, fallbackIdx) : "";
}

// ---------------------------------------------------------------------------
// Stemming
// ---------------------------------------------------------------------------

std::vector<std::string> Dictionary::getStemVariants(const std::string& word) {
  // Try common French elisions before English stemming, including UTF-8
  // apostrophes. Exact headwords have already been tried by the caller.
  // Keep the remainder intact: French accents and endings are not English stems.
  size_t prefixLength = 0;
  if (word.size() >= 3) {
    const int first = std::tolower(static_cast<unsigned char>(word[0]));
    if (first == 'q' && std::tolower(static_cast<unsigned char>(word[1])) == 'u') {
      prefixLength = 2;
    } else if (first != 0 && strchr("cdjlmnst", first)) {
      prefixLength = 1;
    }
  }
  if (prefixLength != 0) {
    size_t apostropheLength = 0;
    if (word[prefixLength] == '\'') {
      apostropheLength = 1;
    } else if (word.compare(prefixLength, 3, "’") == 0) {
      apostropheLength = 3;
    } else if (word.compare(prefixLength, 2, "ʼ") == 0) {
      apostropheLength = 2;
    }
    if (apostropheLength != 0 && prefixLength + apostropheLength < word.size()) {
      return {word.substr(prefixLength + apostropheLength)};
    }
  }

  // The remaining rules are deliberately English-only. Applying them to UTF-8
  // text can manufacture meaningless variants from another language.
  if (std::any_of(word.begin(), word.end(), [](const unsigned char c) { return c >= 0x80; })) return {};

  std::string normalized = word;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  std::vector<std::string> variants;
  variants.reserve(8);
  size_t len = normalized.size();
  if (len < 3) return variants;

  auto endsWith = [&normalized, len](const char* suffix) {
    size_t slen = strlen(suffix);
    return len >= slen && normalized.compare(len - slen, slen, suffix) == 0;
  };

  std::string scratch;
  scratch.reserve(len + 4);
  auto addVariant = [&variants, &scratch, &normalized](size_t keep, const char* suffix) {
    scratch.assign(normalized, 0, keep);
    scratch.append(suffix);
    if (scratch.size() < 2) return;
    if (std::find(variants.begin(), variants.end(), scratch) != variants.end()) return;
    variants.push_back(scratch);
  };
  auto addTail = [&variants, &scratch, &normalized, len](size_t start) {
    scratch.assign(normalized, start, len - start);
    if (scratch.size() < 2) return;
    if (std::find(variants.begin(), variants.end(), scratch) != variants.end()) return;
    variants.push_back(scratch);
  };

  // Plurals
  if (endsWith("sses")) addVariant(len - 2, "");
  if (endsWith("ses")) addVariant(len - 2, "is");
  if (endsWith("ies")) {
    addVariant(len - 3, "y");
    addVariant(len - 2, "");
  }
  if (endsWith("ves")) {
    addVariant(len - 3, "f");
    addVariant(len - 3, "fe");
    addVariant(len - 1, "");
  }
  if (endsWith("men")) addVariant(len - 3, "man");
  if (endsWith("es") && !endsWith("sses") && !endsWith("ies") && !endsWith("ves")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
  }
  if (endsWith("s") && !endsWith("ss") && !endsWith("us") && !endsWith("es")) {
    addVariant(len - 1, "");
  }

  // Past tense
  if (endsWith("ied")) {
    addVariant(len - 3, "y");
    addVariant(len - 1, "");
  }
  if (endsWith("ed") && !endsWith("ied")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
    if (len > 4 && normalized[len - 3] == normalized[len - 4]) {
      addVariant(len - 3, "");
    }
  }

  // Progressive
  if (endsWith("ying")) {
    addVariant(len - 4, "ie");
  }
  if (endsWith("ing") && !endsWith("ying")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
    if (len > 5 && normalized[len - 4] == normalized[len - 5]) {
      addVariant(len - 4, "");
    }
  }

  // Adverb
  if (endsWith("ically")) {
    addVariant(len - 6, "ic");
    addVariant(len - 4, "");
  }
  if (endsWith("ally") && !endsWith("ically")) {
    addVariant(len - 4, "al");
    addVariant(len - 2, "");
  }
  if (endsWith("ily") && !endsWith("ally")) {
    addVariant(len - 3, "y");
  }
  if (endsWith("ly") && !endsWith("ily") && !endsWith("ally")) {
    addVariant(len - 2, "");
  }

  // Comparative / superlative
  if (endsWith("ier")) {
    addVariant(len - 3, "y");
  }
  if (endsWith("er") && !endsWith("ier")) {
    addVariant(len - 2, "");
    addVariant(len - 1, "");
    if (len > 4 && normalized[len - 3] == normalized[len - 4]) {
      addVariant(len - 3, "");
    }
  }
  if (endsWith("iest")) {
    addVariant(len - 4, "y");
  }
  if (endsWith("est") && !endsWith("iest")) {
    addVariant(len - 3, "");
    addVariant(len - 2, "");
    if (len > 5 && normalized[len - 4] == normalized[len - 5]) {
      addVariant(len - 4, "");
    }
  }

  // Derivational suffixes
  if (endsWith("ness")) addVariant(len - 4, "");
  if (endsWith("ment")) addVariant(len - 4, "");
  if (endsWith("ful")) addVariant(len - 3, "");
  if (endsWith("less")) addVariant(len - 4, "");
  if (endsWith("able")) {
    addVariant(len - 4, "");
    addVariant(len - 4, "e");
  }
  if (endsWith("ible")) {
    addVariant(len - 4, "");
    addVariant(len - 4, "e");
  }
  if (endsWith("ation")) {
    addVariant(len - 5, "");
    addVariant(len - 5, "e");
    addVariant(len - 5, "ate");
  }
  if (endsWith("tion") && !endsWith("ation")) {
    addVariant(len - 4, "te");
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ion") && !endsWith("tion")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("al") && !endsWith("ial")) {
    addVariant(len - 2, "");
    addVariant(len - 2, "e");
  }
  if (endsWith("ial")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ous")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ive")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ize")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("ise")) {
    addVariant(len - 3, "");
    addVariant(len - 3, "e");
  }
  if (endsWith("en")) {
    addVariant(len - 2, "");
    addVariant(len - 2, "e");
  }

  // Prefix removal
  if (len > 5 && normalized.compare(0, 2, "un") == 0) addTail(2);
  if (len > 6 && normalized.compare(0, 3, "dis") == 0) addTail(3);
  if (len > 6 && normalized.compare(0, 3, "mis") == 0) addTail(3);
  if (len > 6 && normalized.compare(0, 3, "pre") == 0) addTail(3);
  if (len > 7 && normalized.compare(0, 4, "over") == 0) addTail(4);
  if (len > 5 && normalized.compare(0, 2, "re") == 0) addTail(2);

  return variants;
}

// ---------------------------------------------------------------------------
// Fuzzy search (zero persistent RAM — uses findPageBounds for neighbourhood)
// ---------------------------------------------------------------------------

int Dictionary::editDistance(const std::string& a, const std::string& b, int maxDist, int* dp) {
  int m = static_cast<int>(a.size());
  int n = static_cast<int>(b.size());
  if (std::abs(m - n) > maxDist) return maxDist + 1;

  for (int j = 0; j <= n; j++) dp[j] = j;

  for (int i = 1; i <= m; i++) {
    int prev = dp[0];
    dp[0] = i;
    int rowMin = dp[0];
    for (int j = 1; j <= n; j++) {
      int temp = dp[j];
      if (a[i - 1] == b[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
      if (dp[j] < rowMin) rowMin = dp[j];
    }
    if (rowMin > maxDist) return maxDist + 1;
  }
  return dp[n];
}

std::vector<std::string> Dictionary::findSimilar(const std::string& word, int maxResults, const char* cachePath) {
  std::string folderPath = readDictPath(cachePath);
  if (folderPath.empty()) return {};

  const DictInfo info = readInfo(folderPath.c_str());
  const uint8_t suffixBytes = idxEntrySuffixBytes(info);
  DictPaths dp(folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return {};

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t centerStart = 0;
  uint32_t centerEnd = idxFileSize;

  HalFile oft;
  const bool hasOft = suffixBytes == 8 && Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft);
  HalFile qidx;
  QidxHeader qidxHeader;
  uint32_t qidxSample = 0;
  bool hasQidx = false;

  if (hasOft) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &centerStart, &centerEnd);
  } else if (Storage.openFileForRead("DICT", dp.quickIdx().c_str(), qidx)) {
    qidxHeader = readQidxHeader(qidx, idxFileSize);
    hasQidx = qidxHeader.valid && qidxHeader.sampleCount > 0 &&
              binarySearchQuickIndex(qidx, idx, qidxHeader.sampleCount, word.c_str(), idxFileSize, &centerStart,
                                     &centerEnd, &qidxSample);
    if (!hasQidx) qidx.close();
  }

  if (!hasOft && !hasQidx) {
    // Suggestion generation runs on the main activity task. A full .idx scan
    // here blocks input for minutes on large dictionaries, making the device
    // appear frozen. Direct lookup remains available without an accelerator.
    LOG_DBG("DICT", "Skipping suggestions: no usable index accelerator");
    idx.close();
    return {};
  }

  // Extend the OFT scan window by ±7 pages around the found neighbourhood.
  // A QIDX sample already spans 256 entries, so one adjacent sample on either
  // side provides a similarly bounded fuzzy-search window.
  const uint32_t pageSize = (centerEnd > centerStart) ? (centerEnd - centerStart) : 1;
  static constexpr uint32_t PAGE_RADIUS = 7;
  uint32_t scanStart = (centerStart > PAGE_RADIUS * pageSize) ? (centerStart - PAGE_RADIUS * pageSize) : 0;
  uint32_t scanEnd = std::min(idxFileSize, centerEnd + PAGE_RADIUS * pageSize);

  if (hasOft) {
    // Snap scanStart back to the page boundary containing it. OFT entries are
    // monotonically increasing byte offsets, so binary-search for the largest
    // entry <= scanStart instead of walking (and seeking) every entry.
    const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
    const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;

    auto readEntry = [&oft](uint32_t i) -> uint32_t {
      oft.seekSet(OFT_HEADER_SIZE + i * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) != 4) return 0;
      uint32_t entryVal;
      memcpy(&entryVal, raw, 4);
      return entryVal;
    };

    uint32_t snappedStart = 0;
    uint32_t lo = 0;
    uint32_t hi = numEntries;  // search [lo, hi)
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const uint32_t entryVal = readEntry(mid);
      if (entryVal <= scanStart) {
        snappedStart = entryVal;
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    oft.close();

    idx.seekSet(snappedStart);
  } else {
    uint32_t adjacentOffset = 0;
    if (qidxSample > 0 && readQidxOffset(qidx, qidxSample - 1, &adjacentOffset) && adjacentOffset < centerStart) {
      scanStart = adjacentOffset;
    } else {
      scanStart = centerStart;
    }
    if (qidxSample + 2 < qidxHeader.sampleCount) {
      if (readQidxOffset(qidx, qidxSample + 2, &adjacentOffset) && adjacentOffset > centerEnd &&
          adjacentOffset <= idxFileSize) {
        scanEnd = adjacentOffset;
      } else {
        scanEnd = centerEnd;
      }
    } else {
      scanEnd = idxFileSize;
    }
    qidx.close();
    idx.seekSet(scanStart);
  }

  int maxDist = std::max(2, static_cast<int>(word.size()) / 3 + 1);
  auto editDistanceScratch = makeUniqueNoThrow<int[]>(word.size() + 1);
  if (!editDistanceScratch) {
    LOG_ERR("DICT", "OOM: edit-distance scratch");
    idx.close();
    return {};
  }

  // Collect candidate words into one pooled buffer and sort lightweight
  // {offset, distance} records — avoids a heap std::string per candidate.
  std::string pool;
  pool.reserve(256);
  struct Candidate {
    uint16_t offset;
    uint16_t distance;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(maxResults) * 4);

  while (static_cast<uint32_t>(idx.position()) < scanEnd) {
    int len = readWordInto(idx, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t skip[12];
    if (idx.read(skip, suffixBytes) != suffixBytes) break;

    if (len == 0) continue;
    if (cistrcmp(wordBuf, word.c_str()) == 0) continue;

    int dist = editDistance(wordBuf, word, maxDist, editDistanceScratch.get());
    if (dist <= maxDist) {
      if (pool.size() + static_cast<size_t>(len) + 1 > 0xFFFF) break;
      const uint16_t offset = TextPool::append(pool, wordBuf, static_cast<size_t>(len));
      candidates.push_back({offset, static_cast<uint16_t>(dist)});
    }
  }

  idx.close();

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

  std::vector<std::string> results;
  results.reserve(static_cast<size_t>(maxResults));
  for (size_t i = 0; i < candidates.size() && static_cast<int>(results.size()) < maxResults; i++) {
    results.emplace_back(pool.data() + candidates[i].offset);
  }
  return results;
}
