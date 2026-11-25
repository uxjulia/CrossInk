#include "LanguageCache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace language_cache {
namespace {
constexpr uint8_t OWNER[OWNER_SIZE] = {'C', 'I', 'L', 'A', 'N', 'G', 1, 0, 'O', 'W', 'N', 'E', 'R', 0x5a, 0xa5, 0};
constexpr uint32_t COMMITTED = 0x314e414c;
constexpr size_t RECORD_SIZE = 18;
constexpr size_t LINE_SIZE = 2048;
constexpr size_t UNKNOWN_LIMIT = 256;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32); }
void put16(uint8_t* p, uint16_t n) {
  p[0] = n;
  p[1] = n >> 8;
}
void put32(uint8_t* p, uint32_t n) {
  put16(p, n);
  put16(p + 2, n >> 16);
}
void put64(uint8_t* p, uint64_t n) {
  put32(p, n);
  put32(p + 4, n >> 32);
}
constexpr auto crcTable() {
  std::array<uint32_t, 256> table{};
  for (size_t i = 0; i < table.size(); ++i) {
    uint32_t crc = static_cast<uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    table[i] = crc;
  }
  return table;
}
uint32_t crcByte(uint32_t crc, uint8_t byte) {
  static constexpr auto TABLE = crcTable();  // flash, not a 1 KiB DRAM reservation
  return (crc >> 8) ^ TABLE[(crc ^ byte) & 0xff];
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }

// Cold-path workspace: <5 KiB, allocated once per inspection/install. Too large
// for an activity stack; keeping it resident would penalize C3 reading sessions.
struct Workspace {
  char line[LINE_SIZE];
  uint64_t unknown[UNKNOWN_LIMIT];
  uint8_t seen[MAX_KEYS / 8] = {};
  uint8_t header[HEADER_SIZE];
  uint8_t inputBuffer[256];
  size_t unknownCount = 0;
};
static_assert(sizeof(Workspace) < 5120);

class Reader {
  Input input;
  const ValidationOptions* options;
  uint8_t* buffer;
  size_t cursor = 0, available = 0, total = 0;
  const char* error = nullptr;
  Result fail(Result result, const char* reason) {
    error = reason;
    return result;
  }

 public:
  Reader(Input in, uint8_t* buf, const ValidationOptions* opts = nullptr) : input(in), options(opts), buffer(buf) {}
  const char* errorReason() const { return error; }
  size_t offset() const { return total; }
  Result line(char* out, bool& eof) {
    size_t length = 0;
    eof = false;
    while (true) {
      if (cursor == available) {
        if (options && options->continueWork && !options->continueWork(options->context)) return Result::Cancelled;
        const int n = input.read(input.context, buffer, 256);
        if (n < 0 || n > 256) return fail(Result::Io, "source read failed");
        cursor = 0;
        available = static_cast<size_t>(n);
        if (n == 0) {
          eof = length == 0;
          break;
        }
      }
      if (++total > MAX_SOURCE_SIZE) return fail(Result::TooLarge, "source exceeds byte limit");
      const char c = static_cast<char>(buffer[cursor++]);
      if (c == '\0') return fail(Result::Invalid, "NUL byte in source");
      if (c == '\n') break;
      if (length == LINE_SIZE - 1) return fail(Result::TooLarge, "line exceeds byte limit");
      out[length++] = c;
    }
    if (length && out[length - 1] == '\r') --length;
    out[length] = '\0';
    return validUtf8(out, length) ? Result::Ok : fail(Result::Invalid, "invalid UTF-8 or control character");
  }
};

Result parseLine(char* line, char*& key, char*& value) {
  key = value = nullptr;
  char* p = line;
  while (space(*p)) ++p;
  if (*p == '\0' || *p == '#') return Result::Ok;
  if (!letter(*p) && *p != '_') return Result::Invalid;
  key = p++;
  while (letter(*p) || digit(*p) || *p == '_') ++p;
  char* end = p;
  if (end - key > 127) return Result::TooLarge;
  while (space(*p)) ++p;
  if (*p++ != ':') return Result::Invalid;
  *end = '\0';
  while (space(*p)) ++p;
  if (*p++ != '"') return Result::Invalid;
  value = p;
  char* dest = value;
  while (*p && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      c = *p++;
      if (c == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c != '\\' && c != '"')
        return Result::Invalid;
    }
    *dest++ = c;
  }
  if (*p++ != '"') return Result::Invalid;
  while (space(*p)) ++p;
  if (*p && *p != '#') return Result::Invalid;
  *dest = '\0';
  return validUtf8(value, static_cast<size_t>(dest - value)) ? Result::Ok : Result::Invalid;
}

bool copyCode(char* dest, char* source) {
  for (char* p = source; *p; ++p)
    if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
  if (!validCode(source)) return false;
  std::strcpy(dest, source);
  return true;
}
Result metadataLine(const char* key, char* value, Metadata& meta, bool& explicitDirection) {
  if (std::strcmp(key, "_language_code") == 0) return copyCode(meta.code, value) ? Result::Ok : Result::Invalid;
  if (std::strcmp(key, "_language_name") == 0) {
    if (!*value || std::strlen(value) >= NAME_SIZE || std::strchr(value, '\n') || std::strchr(value, '\t'))
      return Result::Invalid;
    std::strcpy(meta.name, value);
  } else if (std::strcmp(key, "_keyboard") == 0) {
    if (!copyCode(meta.keyboard, value)) return Result::Invalid;
  } else if (std::strcmp(key, "_direction") == 0) {
    if (std::strcmp(value, "rtl") != 0 && std::strcmp(value, "ltr") != 0) return Result::Invalid;
    meta.rtl = std::strcmp(value, "rtl") == 0;
    explicitDirection = true;
  }
  return Result::Ok;
}
Result finishMetadata(Metadata& meta, bool explicitDirection) {
  if (!validCode(meta.code) || !*meta.name) return Result::MetadataMissing;
  if (std::strcmp(meta.code, "EN") == 0) return Result::Invalid;
  if (!explicitDirection) {
    meta.rtl =
        std::strcmp(meta.code, "AR") == 0 || std::strcmp(meta.code, "HE") == 0 || std::strcmp(meta.code, "FA") == 0;
  }
  if (!*meta.keyboard) std::strcpy(meta.keyboard, meta.code);
  return Result::Ok;
}
bool findKey(const Schema& schema, uint64_t h, Key& out) {
  size_t first = 0, last = schema.count;
  while (first < last) {
    const size_t mid = first + (last - first) / 2;
    const Key candidate = schema.at(mid);
    if (candidate.hash < h)
      first = mid + 1;
    else
      last = mid;
  }
  if (first == schema.count) return false;
  out = schema.at(first);
  return out.hash == h && out.id < schema.count;
}
Result remember(Workspace& work, uint64_t h, const Key* known) {
  if (known) {
    const uint8_t bit = uint8_t(1u << (known->id % 8));
    if (work.seen[known->id / 8] & bit) return Result::Duplicate;
    work.seen[known->id / 8] |= bit;
  } else {
    for (size_t i = 0; i < work.unknownCount; ++i)
      if (work.unknown[i] == h) return Result::Duplicate;
    if (work.unknownCount == UNKNOWN_LIMIT) return Result::TooLarge;
    work.unknown[work.unknownCount++] = h;
  }
  return Result::Ok;
}
// Compare serialized compiled records in order, using existing header scratch.
// This stays linear even for reordered input; a different record order is a
// changed compiled image and follows the normal installation path.
struct Comparison {
  const Flash& flash;
  size_t base, used, next = HEADER_SIZE;
  bool equal = true;

  Result record(const uint8_t* record, const char* value, size_t size, uint8_t* scratch) {
    if (!equal) return Result::Ok;
    if (next > used || used - next < RECORD_SIZE) {
      equal = false;
      return Result::Ok;
    }
    if (!flash.read(flash.context, base + next, scratch, RECORD_SIZE)) return Result::Io;
    if (std::memcmp(scratch, record, RECORD_SIZE) != 0) {
      equal = false;
      return Result::Ok;
    }
    if (size > used - next - RECORD_SIZE) return Result::Invalid;
    for (size_t offset = 0; offset < size; offset += HEADER_SIZE) {
      const size_t n = std::min(HEADER_SIZE, size - offset);
      if (!flash.read(flash.context, base + next + RECORD_SIZE + offset, scratch, n)) return Result::Io;
      if (std::memcmp(scratch, value + offset, n) != 0) {
        equal = false;
        break;
      }
    }
    next += RECORD_SIZE + size;
    return Result::Ok;
  }
};
bool writeVerified(const Flash& flash, size_t offset, const void* bytes, size_t size);
// One parser serves dry-run validation and installation; only the installer
// supplies the optional flash sink. Manual installation retains format fallback.
Result parseDocument(Input input, const Schema& schema, Workspace* work, Metadata& metadata, size_t& end,
                     uint16_t& count, const ValidationOptions* options, const Flash* flash = nullptr, size_t base = 0,
                     Comparison* comparison = nullptr) {
  Reader reader(input, work->inputBuffer, options);
  Result result;
  bool sourceMatched = !options || !options->sourceSha256;
  bool keysetMatched = !options || !options->keysetSha256;
  bool direction = false, haveStrings = false;
  end = HEADER_SIZE;
  count = 0;
  while (true) {
    bool eof;
    result = reader.line(work->line, eof);
    if (result != Result::Ok) return result;
    if (eof) break;
    char *key, *value;
    result = parseLine(work->line, key, value);
    if (result != Result::Ok) return result;
    if (!key) continue;
    Key known{};
    const uint64_t h = hash(key);
    const bool found = key[0] != '_' && findKey(schema, h, known) && std::strcmp(known.name, key) == 0;
    result = remember(*work, h, found ? &known : nullptr);
    if (result != Result::Ok) return result;
    if (key[0] == '_') {
      if (haveStrings) return Result::Invalid;
      if (options && std::strcmp(key, "_template_source_sha256") == 0 && options->sourceSha256)
        sourceMatched = std::strcmp(value, options->sourceSha256) == 0;
      if (options && std::strcmp(key, "_template_keyset_sha256") == 0 && options->keysetSha256)
        keysetMatched = std::strcmp(value, options->keysetSha256) == 0;
      result = metadataLine(key, value, metadata, direction);
      if (result != Result::Ok) return result;
      continue;
    }
    if (std::strncmp(key, "STR_", 4) != 0) return Result::Invalid;
    haveStrings = true;
    if (!found || !*value) continue;
    const char* english = schema.english(known.id);
#if !defined(CROSSINK_LANGUAGE_BENCHMARK)
    if (std::strcmp(english, value) == 0) continue;
#endif
    if (known.formatted && !compatibleFormat(english, value)) continue;
    const size_t size = std::strlen(value) + 1;
    if (end + RECORD_SIZE + size > SLOT_SIZE || end + RECORD_SIZE >= MISSING) return Result::TooLarge;
    uint8_t record[RECORD_SIZE];
    put64(record, h);
    put64(record + 8, known.englishHash);
    put16(record + 16, static_cast<uint16_t>(size));
    if (comparison) {
      result = comparison->record(record, value, size, work->header);
      if (result != Result::Ok) return result;
    }
    if (flash && (!writeVerified(*flash, base + end, record, sizeof(record)) ||
                  !writeVerified(*flash, base + end + RECORD_SIZE, value, size)))
      return Result::Io;
    end += RECORD_SIZE + size;
    ++count;
  }
  result = finishMetadata(metadata, direction);
  if (result != Result::Ok) return result;
  if (!sourceMatched || !keysetMatched ||
      (options && options->expectedCode && std::strcmp(metadata.code, options->expectedCode)))
    return Result::Invalid;
  return Result::Ok;
}

bool fieldString(const uint8_t* p, size_t size) {
  const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, size));
  return end && validUtf8(reinterpret_cast<const char*>(p), static_cast<size_t>(end - p));
}
bool headerValid(const uint8_t* header) {
  return std::memcmp(header, OWNER, OWNER_SIZE) == 0 && u32(header + 16) == COMMITTED && u64(header + 20) != 0 &&
         u32(header + 28) >= HEADER_SIZE && u32(header + 28) <= SLOT_SIZE && u16(header + 32) <= MAX_KEYS &&
         header[34] <= 1 && fieldString(header + 40, CODE_SIZE) &&
         validCode(reinterpret_cast<const char*>(header + 40)) &&
         std::strcmp(reinterpret_cast<const char*>(header + 40), "EN") != 0 && fieldString(header + 72, NAME_SIZE) &&
         header[72] != 0 && fieldString(header + 168, CODE_SIZE) &&
         validCode(reinterpret_cast<const char*>(header + 168));
}
void decodeHeader(const uint8_t* header, Installed& result) {
  result.generation = u64(header + 20);
  result.checksum = u32(header + 36);
  std::memcpy(result.metadata.code, header + 40, CODE_SIZE);
  std::memcpy(result.metadata.name, header + 72, NAME_SIZE);
  std::memcpy(result.metadata.keyboard, header + 168, CODE_SIZE);
  result.metadata.rtl = header[34] != 0;
}
Result readCrc(const Flash& flash, size_t base, size_t size, uint8_t* scratch, uint32_t& result) {
  uint32_t crc = ~0u;
  for (size_t offset = 20; offset < size;) {
    const size_t n = std::min<size_t>(256, size - offset);
    if (!flash.read(flash.context, base + offset, scratch, n)) return Result::Io;
    for (size_t i = 0; i < n; ++i) crc = crcByte(crc, offset + i >= 36 && offset + i < 40 ? 0 : scratch[i]);
    offset += n;
  }
  result = ~crc;
  return Result::Ok;
}
bool writeVerified(const Flash& flash, size_t offset, const void* bytes, size_t size) {
  if (!flash.write(flash.context, offset, bytes, size)) return false;
  if (flash.verify) return flash.verify(flash.context, offset, bytes, size);
  uint8_t check[128];
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; i += sizeof(check)) {
    const size_t n = std::min(sizeof(check), size - i);
    if (!flash.read(flash.context, offset + i, check, n) || std::memcmp(check, data + i, n) != 0) return false;
  }
  return true;
}
bool formatSignature(const char* text, uint8_t* signature, size_t& count) {
  count = 0;
  const auto arg = [&](uint8_t type) {
    if (count == 32) return false;
    signature[count++] = type;
    return true;
  };
  for (const char* p = text; *p;) {
    if (*p++ != '%') continue;
    if (*p == '%') {
      ++p;
      continue;
    }
    while (*p && std::strchr("-+ #0", *p)) ++p;
    if (*p == '*') {
      if (!arg(1)) return false;
      ++p;
    } else {
      unsigned width = 0;
      while (digit(*p)) {
        width = width * 10 + (*p++ - '0');
        if (width > 1024) return false;
      }
    }
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        if (!arg(1)) return false;
        ++p;
      } else {
        unsigned precision = 0;
        while (digit(*p)) {
          precision = precision * 10 + (*p++ - '0');
          if (precision > 128) return false;
        }
      }
    }
    uint8_t length = 0;
    if (*p == 'h') {
      ++p;
      length = 1;
      if (*p == 'h') {
        ++p;
        length = 2;
      }
    } else if (*p == 'l') {
      ++p;
      length = 3;
      if (*p == 'l') {
        ++p;
        length = 4;
      }
    } else if (*p == 'j') {
      ++p;
      length = 5;
    } else if (*p == 'z') {
      ++p;
      length = 6;
    } else if (*p == 't') {
      ++p;
      length = 7;
    } else if (*p == 'L') {
      ++p;
      length = 8;
    }
    const char conversion = *p;
    if (!conversion || conversion == 'n') return false;
    ++p;
    uint8_t type = 0;
    if (conversion == 'd' || conversion == 'i')
      type = 1;
    else if (std::strchr("ouxX", conversion))
      type = 2;
    else if (std::strchr("aAeEfFgG", conversion))
      type = 3;
    else if (conversion == 'c')
      type = 4;
    else if (conversion == 's')
      type = 5;
    else if (conversion == 'p')
      type = 6;
    else
      return false;  // includes positional arguments and unknown extensions
    if (!arg(uint8_t(type + length * 8))) return false;
  }
  return true;
}
}  // namespace

uint64_t hash(const char* text) {
  uint64_t value = 14695981039346656037ull;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) value = (value ^ *p) * 1099511628211ull;
  return value;
}
bool validCode(const char* code) {
  const size_t n = std::strlen(code);
  if (!n || n >= CODE_SIZE || !letter(code[0])) return false;
  for (size_t i = 0; i < n; ++i)
    if (!letter(code[i]) && !digit(code[i]) && code[i] != '-' && code[i] != '_') return false;
  return true;
}
bool validUtf8(const char* text, size_t length) {
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i < length;) {
    uint32_t cp = p[i++];
    if (cp < 0x80) {
      if (cp == 0 || (cp < 0x20 && cp != '\n' && cp != '\t')) return false;
      continue;
    }
    unsigned extra;
    uint32_t minimum;
    if (cp >= 0xc2 && cp <= 0xdf) {
      extra = 1;
      minimum = 0x80;
      cp &= 0x1f;
    } else if (cp >= 0xe0 && cp <= 0xef) {
      extra = 2;
      minimum = 0x800;
      cp &= 0x0f;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
      extra = 3;
      minimum = 0x10000;
      cp &= 7;
    } else
      return false;
    if (extra > length - i) return false;
    while (extra--) {
      const uint8_t c = p[i++];
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}
bool compatibleFormat(const char* english, const char* translated) {
  uint8_t a[32], b[32];
  size_t an, bn;
  return formatSignature(english, a, an) && formatSignature(translated, b, bn) && an == bn &&
         std::memcmp(a, b, an) == 0;
}
const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Unchanged:
      return "language already active";
    case Result::Io:
      return "read/write failed";
    case Result::Memory:
      return "out of memory";
    case Result::Invalid:
      return "invalid language file";
    case Result::TooLarge:
      return "language file exceeds limits";
    case Result::Duplicate:
      return "duplicate key";
    case Result::MetadataMissing:
      return "missing language metadata";
    case Result::Cancelled:
      return "cancelled";
    case Result::RestartRequired:
      return "restart required before another language update";
    case Result::StorageUnavailable:
      return "language cache partition unavailable";
  }
  return "unknown";
}
size_t slotOffset(size_t size, int slot) { return size - 2 * SLOT_SIZE + static_cast<size_t>(slot) * SLOT_SIZE; }

struct Inspector::Impl : Workspace {};
Inspector::Inspector() { workspace_.init(MemoryPool::None); }
Inspector::~Inspector() = default;

Result inspect(Input input, Metadata& metadata, InspectionDiagnostic* diagnostic) {
  Inspector inspector;
  return inspector.inspect(input, metadata, diagnostic);
}
Result Inspector::inspect(Input input, Metadata& metadata, InspectionDiagnostic* diagnostic) {
  if (diagnostic) *diagnostic = {};
  auto* work = workspace_.get();
  if (!work || !input.read) {
    if (diagnostic) diagnostic->reason = work ? "missing source reader" : "metadata workspace allocation failed";
    return work ? Result::Invalid : Result::Memory;
  }
  work->unknownCount = 0;
  metadata = {};
  Reader reader(input, work->inputBuffer);
  size_t line = 0;
  const auto reject = [&](Result result, const char* reason) {
    if (diagnostic) *diagnostic = {reason, line, reader.offset()};
    return result;
  };
  bool direction = false;
  while (true) {
    ++line;
    bool eof;
    auto result = reader.line(work->line, eof);
    if (result != Result::Ok) return reject(result, reader.errorReason());
    if (eof) break;
    if (line == 1 && std::strncmp(work->line, "\xef\xbb\xbf", 3) == 0)
      return reject(Result::Invalid, "UTF-8 BOM is not supported");
    char *key, *value;
    result = parseLine(work->line, key, value);
    if (result != Result::Ok)
      return reject(result, result == Result::TooLarge ? "YAML key exceeds byte limit"
                                                       : "invalid YAML syntax: expected key: \"value\"");
    if (!key) continue;
    if (key[0] != '_') break;
    result = remember(*work, hash(key), nullptr);
    if (result != Result::Ok)
      return reject(result, result == Result::Duplicate ? "duplicate metadata key" : "metadata key limit exceeded");
    result = metadataLine(key, value, metadata, direction);
    if (result != Result::Ok) return reject(result, "invalid language metadata value");
  }
  const auto result = finishMetadata(metadata, direction);
  return result == Result::Ok
             ? result
             : reject(result, result == Result::MetadataMissing
                                  ? "missing _language_code or _language_name before translation strings"
                                  : "English is built in; EN is not an SD language");
}

Result validate(Input input, const Schema& schema, Metadata& metadata, const ValidationOptions& options) {
  metadata = {};
  if (!input.read || schema.count > MAX_KEYS || !schema.at || !schema.english) return Result::Invalid;
  HeapObject<Workspace> storage;
  if (!storage.init(MemoryPool::None)) return Result::Memory;
  size_t end;
  uint16_t count;
  Metadata candidate;
  const auto result = parseDocument(input, schema, storage.get(), candidate, end, count, &options);
  if (result == Result::Ok) metadata = candidate;
  return result;
}

Result install(Input input, const Schema& schema, const Flash& flash, int pinnedSlot, Installed& installed,
               uint8_t* staging) {
  if (schema.count > MAX_KEYS || pinnedSlot < -1 || pinnedSlot > 1) return Result::Invalid;
  if (flash.size < 2 * SLOT_SIZE || flash.size % SLOT_SIZE != 0) return Result::StorageUnavailable;
  HeapObject<Workspace> storage;
  if (!storage.init(MemoryPool::None)) return Result::Memory;
  auto* work = storage.get();
  Result result;
  // The HAL supplies firmware-owned data storage. Legacy filesystem bytes do
  // not prevent provisioning; only a committed language cache is reusable.
  uint64_t newest = 0;
  int latestSlot = -1;
  size_t activeUsed = 0;
  uint16_t activeCount = 0;
  for (int slot = 0; slot < 2; ++slot) {
    const size_t base = slotOffset(flash.size, slot);
    if (!flash.read(flash.context, base, work->header, HEADER_SIZE)) return Result::Io;
    if (!headerValid(work->header)) continue;
    const uint64_t gen = u64(work->header + 20);
    const uint32_t expected = u32(work->header + 36);
    uint32_t crc;
    result = readCrc(flash, base, u32(work->header + 28), work->inputBuffer, crc);
    if (result != Result::Ok) return result;
    if (crc == expected && slot == pinnedSlot) {
      decodeHeader(work->header, installed);
      installed.slot = slot;
      activeUsed = u32(work->header + 28);
      activeCount = u16(work->header + 32);
    }
    if (crc == expected && gen > newest) {
      newest = gen;
      latestSlot = slot;
    }
  }
  Metadata metadata;
  size_t end;
  uint16_t count;
  if (pinnedSlot >= 0) {
    if (!activeUsed) return Result::Invalid;
    Comparison comparison{flash, slotOffset(flash.size, pinnedSlot), activeUsed};
    result = parseDocument(input, schema, work, metadata, end, count, nullptr, nullptr, 0, &comparison);
    if (result != Result::Ok) return result;
    if (comparison.equal && end == activeUsed && count == activeCount &&
        std::strcmp(metadata.code, installed.metadata.code) == 0 &&
        std::strcmp(metadata.name, installed.metadata.name) == 0 &&
        std::strcmp(metadata.keyboard, installed.metadata.keyboard) == 0 && metadata.rtl == installed.metadata.rtl)
      return Result::Unchanged;
    // Rewind the same open source before erasing: a missing/failed rewind must
    // leave both flash slots untouched. No second reader of the SD path is opened.
    if (!input.rewind || !input.rewind(input.context)) return Result::Io;
    std::memset(work->seen, 0, sizeof(work->seen));
    work->unknownCount = 0;
    metadata = {};
  }
  if (newest == std::numeric_limits<uint64_t>::max()) return Result::Invalid;
  const int target = pinnedSlot >= 0 ? 1 - pinnedSlot : (latestSlot >= 0 ? 1 - latestSlot : 0);
  const size_t base = slotOffset(flash.size, target);
  // Compile into caller-owned RAM when supplied. The selected old slot is never
  // touched, and parse failures do not modify storage at all in this mode.
  struct StagedSlot {
    uint8_t* bytes;
    size_t base;
    bool contains(size_t offset, size_t length) const {
      return offset >= base && offset - base <= SLOT_SIZE && length <= SLOT_SIZE - (offset - base);
    }
  } staged{staging, base};
  const Flash buffered{&staged, flash.size,
                       [](void* context, size_t offset, void* bytes, size_t length) {
                         auto& slot = *static_cast<StagedSlot*>(context);
                         if (!slot.contains(offset, length)) return false;
                         std::memcpy(bytes, slot.bytes + offset - slot.base, length);
                         return true;
                       },
                       [](void* context, size_t offset, const void* bytes, size_t length) {
                         auto& slot = *static_cast<StagedSlot*>(context);
                         if (!slot.contains(offset, length)) return false;
                         std::memcpy(slot.bytes + offset - slot.base, bytes, length);
                         return true;
                       },
                       [](void* context, size_t offset, size_t length) {
                         auto& slot = *static_cast<StagedSlot*>(context);
                         if (!slot.contains(offset, length)) return false;
                         std::memset(slot.bytes + offset - slot.base, 0xff, length);
                         return true;
                       }};
  const Flash& output = staging ? buffered : flash;
  if (!output.erase(output.context, base, SLOT_SIZE) || !writeVerified(output, base, OWNER, OWNER_SIZE))
    return Result::Io;
  result = parseDocument(input, schema, work, metadata, end, count, nullptr, &output, base);
  if (result != Result::Ok) return result;
  std::memset(work->header, 0, HEADER_SIZE);
  std::memcpy(work->header, OWNER, OWNER_SIZE);
  std::memset(work->header + 16, 0xff, 4);  // publish last
  put64(work->header + 20, newest + 1);
  put32(work->header + 28, static_cast<uint32_t>(end));
  put16(work->header + 32, count);
  work->header[34] = metadata.rtl;
  std::memcpy(work->header + 40, metadata.code, CODE_SIZE);
  std::memcpy(work->header + 72, metadata.name, NAME_SIZE);
  std::memcpy(work->header + 168, metadata.keyboard, CODE_SIZE);
  // Leave checksum erased too; flash cannot program zero bits back to one.
  std::memset(work->header + 36, 0xff, 4);
  if (!writeVerified(output, base + 20, work->header + 20, HEADER_SIZE - 20)) return Result::Io;
  uint32_t crc;
  result = readCrc(output, base, end, work->inputBuffer, crc);
  if (result != Result::Ok) return result;
  uint8_t encoded[4];
  put32(encoded, crc);
  if (!writeVerified(output, base + 36, encoded, sizeof(encoded))) return Result::Io;
  // Persist and verify the uncommitted image first. A failed/torn write cannot
  // make this slot bootable. The commit marker is a separate durable write.
  if (staging && (!flash.erase(flash.context, base, SLOT_SIZE) || !writeVerified(flash, base, staging, end)))
    return Result::Io;
  put32(encoded, COMMITTED);
  if (!writeVerified(flash, base + 16, encoded, sizeof(encoded))) return Result::Io;
  installed.metadata = metadata;
  installed.generation = newest + 1;
  installed.checksum = crc;
  installed.slot = target;
  return Result::Ok;
}

bool open(const uint8_t* bytes, size_t length, const Schema& schema, Installed& installed, uint16_t* offsets) {
  if (!bytes || length < HEADER_SIZE || schema.count > MAX_KEYS || !headerValid(bytes)) return false;
  const size_t used = u32(bytes + 28);
  if (used > length) return false;
  uint32_t crc = ~0u;
  for (size_t i = 20; i < used; ++i) crc = crcByte(crc, i >= 36 && i < 40 ? 0 : bytes[i]);
  if (~crc != u32(bytes + 36)) return false;
  // Check active-key duplicates with a bounded bitset; historical keys that
  // this firmware does not know cannot populate an offset. Never scan earlier
  // records here: wake-time work must remain O(n log n), not quadratic.
  uint8_t seen[MAX_KEYS / 8] = {};
  size_t pos = HEADER_SIZE;
  const size_t count = u16(bytes + 32);
  for (size_t i = 0; i < count; ++i) {
    if (used - pos < RECORD_SIZE) return false;
    const size_t n = u16(bytes + pos + 16);
    if (!n || n > used - pos - RECORD_SIZE || pos + RECORD_SIZE >= MISSING) return false;
    const char* text = reinterpret_cast<const char*>(bytes + pos + RECORD_SIZE);
    if (text[n - 1] != '\0' || !validUtf8(text, n - 1)) return false;
    Key known{};
    if (findKey(schema, u64(bytes + pos), known)) {
      const uint8_t bit = uint8_t(1u << (known.id % 8));
      if (seen[known.id / 8] & bit) return false;
      seen[known.id / 8] |= bit;
    }
    pos += RECORD_SIZE + n;
  }
  if (pos != used) return false;
  if (offsets) {
    std::fill_n(offsets, schema.count, MISSING);
    pos = HEADER_SIZE;
    for (size_t i = 0; i < count; ++i) {
      Key key{};
      const char* text = reinterpret_cast<const char*>(bytes + pos + RECORD_SIZE);
      if (findKey(schema, u64(bytes + pos), key) && key.englishHash == u64(bytes + pos + 8) &&
          (!key.formatted || compatibleFormat(schema.english(key.id), text)))
        offsets[key.id] = static_cast<uint16_t>(pos + RECORD_SIZE);
      pos += RECORD_SIZE + u16(bytes + pos + 16);
    }
  }
  decodeHeader(bytes, installed);
  return true;
}
}  // namespace language_cache
