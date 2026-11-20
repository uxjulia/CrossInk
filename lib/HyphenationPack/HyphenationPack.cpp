#include "HyphenationPack.h"

#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace hyphenation_pack {
namespace {
constexpr uint32_t COMMITTED = 0x50485950;
constexpr size_t ENTRY_OFFSET = 32;
constexpr size_t ENTRY_SIZE = 20;
constexpr size_t BUFFER_SIZE = 1024;
static_assert(ENTRY_OFFSET + MAX_PACKS * ENTRY_SIZE <= HEADER_SIZE);

// One 1280-byte cold-path workspace; too large for the C3 task stack and
// unnecessary as permanent RAM. Reused for all checks/copies in an operation.
struct Workspace {
  uint8_t header[HEADER_SIZE];
  uint8_t buffer[BUFFER_SIZE];
};
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
size_t align4(size_t n) { return (n + 3) & ~size_t{3}; }
Entry decode(const uint8_t* p) {
  Entry entry;
  entry.code[0] = static_cast<char>(p[0]);
  entry.code[1] = static_cast<char>(p[1]);
  entry.minPrefix = p[2];
  entry.minSuffix = p[3];
  entry.offset = u32(p + 4);
  entry.size = u32(p + 8);
  entry.rootOffset = u32(p + 12);
  entry.checksum = u32(p + 16);
  return entry;
}
void encode(uint8_t* p, const Entry& entry) {
  p[0] = entry.code[0];
  p[1] = entry.code[1];
  p[2] = entry.minPrefix;
  p[3] = entry.minSuffix;
  put32(p + 4, entry.offset);
  put32(p + 8, entry.size);
  put32(p + 12, entry.rootOffset);
  put32(p + 16, entry.checksum);
}
uint32_t headerCrc(const uint8_t* header) {
  uint32_t crc = crc32(header, 20);
  const uint8_t zeros[4] = {};
  crc = crc32(zeros, sizeof(zeros), crc);
  // The commit marker is programmed last and excluded from the checksum.
  return crc32(header + 28, HEADER_SIZE - 28, crc);
}
bool validHeader(const uint8_t* header, size_t bankSize, Supported supported) {
  if (std::memcmp(header, "CIHP\1\0\0\0", 8) || u32(header + 24) != COMMITTED || !u32(header + 8) ||
      u32(header + 12) > MAX_PACKS || u32(header + 28) || u32(header + 20) != headerCrc(header))
    return false;
  size_t end = SECTOR_SIZE;
  for (size_t i = 0; i < u32(header + 12); ++i) {
    const Entry entry = decode(header + ENTRY_OFFSET + i * ENTRY_SIZE);
    if (!supported(entry.code, entry.minPrefix, entry.minSuffix) || entry.offset != end || !entry.size ||
        entry.rootOffset >= entry.size || entry.offset > bankSize || entry.size > bankSize - entry.offset)
      return false;
    for (size_t j = 0; j < i; ++j)
      if (!std::memcmp(entry.code, header + ENTRY_OFFSET + j * ENTRY_SIZE, 2)) return false;
    end = align4(entry.offset + entry.size);
  }
  return end == u32(header + 16) && end <= bankSize;
}
bool checkPayload(const Flash& flash, size_t offset, size_t size, uint32_t expected, uint8_t* buffer) {
  uint32_t crc = 0;
  for (size_t copied = 0; copied < size;) {
    const size_t n = std::min(BUFFER_SIZE, size - copied);
    if (!flash.read(flash.context, offset + copied, buffer, n)) return false;
    crc = crc32(buffer, n, crc);
    copied += n;
    if (flash.yield && copied % (16 * BUFFER_SIZE) == 0) flash.yield();
  }
  return crc == expected;
}
bool validPayloads(const Flash& flash, size_t bankOffset, const uint8_t* header, uint8_t* buffer) {
  for (size_t i = 0; i < u32(header + 12); ++i) {
    const Entry entry = decode(header + ENTRY_OFFSET + i * ENTRY_SIZE);
    if (!checkPayload(flash, bankOffset + entry.offset, entry.size, entry.checksum, buffer)) return false;
  }
  return true;
}
// The source header is shared with the installer; full online admission below
// additionally checks the serialized trie before the SD transaction publishes it.
Result readSourceHeader(Input input, Supported supported, Entry& entry) {
  uint8_t header[PACK_HEADER_SIZE];
  if (!input.read || input.size < sizeof(header)) return Result::Invalid;
  if (!input.read(input.context, 0, header, sizeof(header))) return Result::Io;
  if (std::memcmp(header, "CPHY", 4) || header[4] != 1 || header[9] || header[10] || header[11]) return Result::Invalid;
  entry = {};
  entry.code[0] = header[5];
  entry.code[1] = header[6];
  entry.minPrefix = header[7];
  entry.minSuffix = header[8];
  entry.rootOffset = u32(header + 12);
  entry.size = u32(header + 16);
  entry.checksum = u32(header + 20);
  if (!entry.size || entry.rootOffset >= entry.size || entry.size != input.size - sizeof(header))
    return Result::Invalid;
  if (!supported || !supported(entry.code, entry.minPrefix, entry.minSuffix)) return Result::Unsupported;
  return Result::Ok;
}

constexpr size_t NODE_WINDOW = 65536;
constexpr size_t MAX_DEPTH = 160;  // Reader's maximum augmented word length.
struct Node {
  uint32_t address = 0;
  uint32_t targets = 0;
  uint32_t end = 0;
  uint16_t children = 0;
  uint16_t next = 0;
  uint8_t stride = 0;
};
// Bounded cold-path allocation (~13 KiB): node stack avoids recursion, bitmap
// windows prove child offsets name node starts without loading the whole pack.
// Four windows cover the maximum source; no permanently resident RAM is used.
struct ValidationWorkspace {
  uint8_t buffer[BUFFER_SIZE];
  uint8_t starts[NODE_WINDOW / 8];
  Node stack[MAX_DEPTH];
};
static_assert(sizeof(ValidationWorkspace) < 14 * 1024);

class SourceReader {
 public:
  Input input;
  const ValidationOptions& options;
  uint8_t* buffer;
  size_t cached = 0;
  size_t cachedSize = 0;
  Result result = Result::Ok;
  size_t pollCount = 0;
  SourceReader(Input in, const ValidationOptions& opts, uint8_t* bytes) : input(in), options(opts), buffer(bytes) {}
  bool tick() {
    // Poll after at most 64 bounded node/cache operations. The caller should
    // check cancellation cheaply and yield by elapsed time, not sleep per poll.
    if ((pollCount++ & 63u) != 0) return result == Result::Ok;
    if (options.continueWork && !options.continueWork(options.context)) result = Result::Cancelled;
    return result == Result::Ok;
  }
  bool byte(size_t offset, uint8_t& value) {
    if (offset >= input.size - PACK_HEADER_SIZE) {
      result = Result::Invalid;
      return false;
    }
    if (offset < cached || offset - cached >= cachedSize) {
      if (!tick()) return false;
      cached = offset;
      cachedSize = std::min(BUFFER_SIZE, input.size - PACK_HEADER_SIZE - cached);
      if (!input.read(input.context, PACK_HEADER_SIZE + cached, buffer, cachedSize)) {
        result = Result::Io;
        return false;
      }
    }
    value = buffer[offset - cached];
    return true;
  }
  bool node(size_t address, Node& node, size_t& levelsEnd) {
    node = {};
    node.address = address;
    uint8_t header;
    if (!byte(address, header)) return false;
    size_t pos = address + 1;
    node.children = header & 31;
    node.stride = (header >> 5) & 3;
    if (!node.stride) node.stride = 1;
    if (node.children == 31) {
      uint8_t count;
      if (!byte(pos++, count)) return false;
      node.children = count;
    }
    if (header & 128) {
      uint8_t hi, lo;
      if (!byte(pos++, hi) || !byte(pos++, lo)) return false;
      const size_t offset = (size_t(hi) << 4) | (lo >> 4);
      if (offset < 4 || offset - 4 + (lo & 15) > address) return false;
      levelsEnd = std::max(levelsEnd, offset - 4 + (lo & 15));
    }
    node.targets = pos + node.children;
    node.end = node.targets + size_t(node.children) * node.stride;
    if (node.end > input.size - PACK_HEADER_SIZE) return false;
    // Duplicate transition bytes are ambiguous. The actual serialized snapshot
    // does not sort every node, so only uniqueness is required.
    uint8_t seen[32] = {};
    for (size_t i = 0; i < node.children; ++i) {
      uint8_t ch;
      if (!byte(pos + i, ch) || (seen[ch / 8] & (1u << (ch % 8)))) return false;
      seen[ch / 8] |= uint8_t(1u << (ch % 8));
    }
    return true;
  }
  bool target(const Node& node, size_t child, size_t& address) {
    uint32_t encoded = 0;
    for (size_t i = 0; i < node.stride; ++i) {
      uint8_t part;
      if (!byte(node.targets + child * node.stride + i, part)) return false;
      encoded = (encoded << 8) | part;
    }
    const int32_t delta = node.stride == 1   ? static_cast<int8_t>(encoded)
                          : node.stride == 2 ? static_cast<int16_t>(encoded)
                                             : static_cast<int32_t>(encoded) - (1 << 23);
    // The supported generated format lays children before parents. Enforcing
    // this rules out cycles and keeps structural traversal finite.
    if (delta >= 0 || int64_t(node.address) + delta < 0) return false;
    address = static_cast<size_t>(int64_t(node.address) + delta);
    return true;
  }
};

Result validateStructure(SourceReader& reader, const Entry& entry, ValidationWorkspace& work) {
  size_t levelsEnd = 0;
  size_t firstNode = entry.rootOffset;
  if (!reader.node(entry.rootOffset, work.stack[0], levelsEnd) || work.stack[0].end != entry.size)
    return reader.result == Result::Ok ? Result::Invalid : reader.result;
  size_t depth = 1;
  // Shared subtries are visited repeatedly without a whole-file visited map.
  // A finite budget bounds malicious branching; all nine existing packs fit.
  size_t budget = size_t(entry.size) * 4;
  while (depth) {
    if (!budget-- || !reader.tick()) return reader.result == Result::Ok ? Result::Invalid : reader.result;
    auto& current = work.stack[depth - 1];
    if (current.next == current.children) {
      --depth;
      continue;
    }
    size_t target;
    if (depth == MAX_DEPTH || !reader.target(current, current.next++, target) ||
        !reader.node(target, work.stack[depth], levelsEnd) || work.stack[depth].end > current.address)
      return reader.result == Result::Ok ? Result::Invalid : reader.result;
    firstNode = std::min(firstNode, target);
    ++depth;
  }
  if (levelsEnd != firstNode) return Result::Invalid;
  // Validate a complete partition of nodes, and then require every child to
  // land exactly on one of those boundaries, using a reusable 8 KiB bitmap.
  for (size_t window = (firstNode / NODE_WINDOW) * NODE_WINDOW; window < entry.size; window += NODE_WINDOW) {
    std::memset(work.starts, 0, sizeof(work.starts));
    for (size_t pass = 0; pass < 2; ++pass) {
      for (size_t pos = firstNode; pos < entry.size;) {
        if (!reader.tick()) return reader.result;
        Node node;
        if (!reader.node(pos, node, levelsEnd)) return reader.result == Result::Ok ? Result::Invalid : reader.result;
        if (pass == 0 && pos >= window && pos - window < NODE_WINDOW) {
          const size_t bit = pos - window;
          work.starts[bit / 8] |= uint8_t(1u << (bit % 8));
        }
        if (pass == 1) {
          for (size_t i = 0; i < node.children; ++i) {
            size_t target;
            if (!reader.target(node, i, target) || target < firstNode)
              return reader.result == Result::Ok ? Result::Invalid : reader.result;
            if (target >= window && target - window < NODE_WINDOW) {
              const size_t bit = target - window;
              if (!(work.starts[bit / 8] & (1u << (bit % 8)))) return Result::Invalid;
            }
          }
        }
        pos = node.end;
      }
    }
  }
  return levelsEnd == firstNode ? Result::Ok : Result::Invalid;
}
}  // namespace

uint32_t crc32(const void* bytes, size_t length, uint32_t previous) {
  auto crc = ~previous;
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (size_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
uint32_t identity(const Entry& entry) {
  uint8_t bytes[ENTRY_SIZE];
  encode(bytes, entry);
  put32(bytes + 4, 0);  // A bank relocation does not change hyphenation.
  const uint32_t result = crc32(bytes, sizeof(bytes));
  return result ? result : 1;  // zero identifies an absent/disabled pattern set
}
const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "installed; restart required";
    case Result::Unchanged:
      return "unchanged";
    case Result::Invalid:
      return "invalid pack or checksum";
    case Result::Unsupported:
      return "unsupported language or word rules";
    case Result::Io:
      return "SD or flash I/O error";
    case Result::Memory:
      return "out of memory";
    case Result::NoSpace:
      return "hyphenation storage full";
    case Result::Unavailable:
      return "hyphenation storage unavailable";
    case Result::Cancelled:
      return "cancelled";
    case Result::RestartRequired:
      return "restart before another update";
  }
  return "unknown error";
}
Result validate(Input input, Supported supported, Entry& entry, const ValidationOptions& options) {
  entry = {};
  if (input.size > MAX_SOURCE_SIZE) return Result::NoSpace;
  if (options.continueWork && !options.continueWork(options.context)) return Result::Cancelled;
  Entry candidate;
  const auto headerResult = readSourceHeader(input, supported, candidate);
  if (headerResult != Result::Ok) return headerResult;
  if (candidate.code[0] < 'a' || candidate.code[0] > 'z' || candidate.code[1] < 'a' || candidate.code[1] > 'z' ||
      candidate.minPrefix != 2 || candidate.minSuffix != 2 ||
      (options.expectedCode && std::strcmp(candidate.code, options.expectedCode)))
    return Result::Invalid;
  if (candidate.size > options.maxPayloadSize) return Result::NoSpace;
  auto work = makeUniqueNoThrow<ValidationWorkspace>();
  if (!work) return Result::Memory;
  SourceReader reader(input, options, work->buffer);
  uint32_t crc = 0;
  for (size_t pos = 0; pos < candidate.size;) {
    if (!reader.tick()) return reader.result;
    const size_t count = std::min(BUFFER_SIZE, size_t(candidate.size) - pos);
    if (!input.read(input.context, PACK_HEADER_SIZE + pos, work->buffer, count)) return Result::Io;
    crc = crc32(work->buffer, count, crc);
    pos += count;
  }
  if (crc != candidate.checksum) return Result::Invalid;
  const auto result = validateStructure(reader, candidate, *work);
  if (result == Result::Ok) entry = candidate;
  return result;
}

bool tailAvailable(Flash flash, size_t bankOffset, size_t bankSize, size_t tailBytes) {
  if (!flash.read || bankSize < HEADER_SIZE || tailBytes > bankSize || bankOffset > flash.size ||
      bankSize > flash.size - bankOffset)
    return false;
  uint8_t header[HEADER_SIZE];
  if (!flash.read(flash.context, bankOffset, header, sizeof(header))) return false;
  if (std::all_of(header, header + HEADER_SIZE, [](uint8_t byte) { return byte == 0xff; })) return true;
  const auto anyPack = [](const char*, uint8_t, uint8_t) { return true; };
  return validHeader(header, bankSize, anyPack) && u32(header + 16) <= bankSize - tailBytes;
}

Result Store::begin(Flash flash, Supported supported, size_t reservedTailBytes) {
  if (ready_) return Result::Unchanged;
  if (!supported || !flash.read || !flash.write || !flash.erase || flash.size < 2 * 65536 || flash.size % (2 * 65536) ||
      flash.size > 16 * 1024 * 1024 || reservedTailBytes % SECTOR_SIZE || reservedTailBytes >= flash.size / 2)
    return Result::Unavailable;
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) return Result::Memory;
  flash_ = flash;
  supported_ = supported;
  bankSize_ = flash.size / 2;
  writeCapacity_ = bankSize_ - reservedTailBytes;
  bank_ = -1;  // A previous attempt may have failed after reading only one bank.
  for (int bank = 0; bank < 2; ++bank) {
    const size_t base = static_cast<size_t>(bank) * bankSize_;
    if (!flash.read(flash.context, base, work->header, HEADER_SIZE)) return Result::Io;
    if (!validHeader(work->header, bankSize_, supported) || !validPayloads(flash, base, work->header, work->buffer))
      continue;
    if (bank_ < 0 || u32(work->header + 8) > u32(header_ + 8)) {
      std::memcpy(header_, work->header, HEADER_SIZE);
      bank_ = bank;
    }
  }
  ready_ = true;
  return Result::Ok;
}
size_t Store::count() const { return bank_ < 0 ? 0 : u32(header_ + 12); }
size_t Store::usedBytes() const { return bank_ < 0 ? 0 : u32(header_ + 16); }
size_t Store::availablePayloadBytes(const char* replacementCode) const {
  if (!ready_ || pending_ || !replacementCode) return 0;
  size_t used = SECTOR_SIZE;
  size_t retained = 0;
  for (size_t i = 0; i < count(); ++i) {
    const auto entry = decode(header_ + ENTRY_OFFSET + i * ENTRY_SIZE);
    if (!std::strcmp(entry.code, replacementCode)) continue;
    used += align4(entry.size);
    ++retained;
  }
  return retained < MAX_PACKS && used <= writeCapacity_ ? writeCapacity_ - used : 0;
}
bool Store::find(const char* code, Entry& entry) const {
  if (!code || std::strlen(code) != 2) return false;
  for (size_t i = 0; i < count(); ++i) {
    const Entry candidate = decode(header_ + ENTRY_OFFSET + i * ENTRY_SIZE);
    if (!std::strcmp(candidate.code, code)) {
      entry = candidate;
      return true;
    }
  }
  return false;
}
Result Store::install(Input input) {
  if (!ready_) return Result::Unavailable;
  if (pending_) return Result::RestartRequired;
  Entry entry;
  const auto result = readSourceHeader(input, supported_, entry);
  if (result != Result::Ok) return result;
  return rewrite(entry.code, &input, &entry);
}
Result Store::remove(const char* code) {
  if (!ready_) return Result::Unavailable;
  if (pending_) return Result::RestartRequired;
  Entry existing;
  if (!find(code, existing)) return Result::Unchanged;
  return rewrite(code, nullptr, nullptr);
}
Result Store::rewrite(const char* code, const Input* input, const Entry* replacement) {
  if (bank_ >= 0 && u32(header_ + 8) == std::numeric_limits<uint32_t>::max()) return Result::Unavailable;
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) return Result::Memory;
  std::memset(work->header, 0, HEADER_SIZE);
  std::memcpy(work->header, "CIHP\1\0\0\0", 8);
  put32(work->header + 8, bank_ < 0 ? 1 : u32(header_ + 8) + 1);
  put32(work->header + 24, 0xffffffffu);
  size_t next = SECTOR_SIZE;
  size_t entries = 0;
  for (size_t i = 0; i < count(); ++i) {
    Entry entry = decode(header_ + ENTRY_OFFSET + i * ENTRY_SIZE);
    if (!std::strcmp(entry.code, code)) continue;
    entry.offset = next;
    encode(work->header + ENTRY_OFFSET + entries++ * ENTRY_SIZE, entry);
    next = align4(next + entry.size);
  }
  if (replacement) {
    if (entries == MAX_PACKS || next > writeCapacity_ || replacement->size > writeCapacity_ - next)
      return Result::NoSpace;
    Entry entry = *replacement;
    entry.offset = next;
    encode(work->header + ENTRY_OFFSET + entries++ * ENTRY_SIZE, entry);
    next = align4(next + entry.size);
    uint32_t crc = 0;
    for (size_t copied = 0; copied < entry.size;) {
      const size_t n = std::min<size_t>(BUFFER_SIZE, entry.size - copied);
      if (!input->read(input->context, PACK_HEADER_SIZE + copied, work->buffer, n)) return Result::Io;
      crc = crc32(work->buffer, n, crc);
      copied += n;
      if (flash_.yield && copied % (16 * BUFFER_SIZE) == 0) flash_.yield();
    }
    if (crc != entry.checksum) return Result::Invalid;
    Entry existing;
    if (find(code, existing) && existing.size == entry.size && existing.rootOffset == entry.rootOffset &&
        existing.minPrefix == entry.minPrefix && existing.minSuffix == entry.minSuffix &&
        existing.checksum == entry.checksum)
      return Result::Unchanged;
  }
  if (next > writeCapacity_) return Result::NoSpace;
  put32(work->header + 12, entries);
  put32(work->header + 16, next);
  put32(work->header + 20, headerCrc(work->header));
  const size_t target = bank_ == 0 ? bankSize_ : 0;
  for (size_t offset = 0; offset < next; offset += SECTOR_SIZE) {
    if (!flash_.erase(flash_.context, target + offset, SECTOR_SIZE)) return Result::Io;
    if (flash_.yield) flash_.yield();
  }
  for (size_t i = 0; i < entries; ++i) {
    const Entry entry = decode(work->header + ENTRY_OFFSET + i * ENTRY_SIZE);
    const bool added = replacement && !std::strcmp(entry.code, code);
    Entry source;
    if (!added && !find(entry.code, source)) return Result::Invalid;
    for (size_t copied = 0; copied < entry.size;) {
      const size_t n = std::min<size_t>(BUFFER_SIZE, entry.size - copied);
      const bool read = added ? input->read(input->context, PACK_HEADER_SIZE + copied, work->buffer, n)
                              : flash_.read(flash_.context, bankOffset() + source.offset + copied, work->buffer, n);
      if (!read || !flash_.write(flash_.context, target + entry.offset + copied, work->buffer, n)) return Result::Io;
      copied += n;
      if (flash_.yield) flash_.yield();
    }
  }
  // Verify the actual flash bytes, including copied old packs, before publishing.
  if (!validPayloads(flash_, target, work->header, work->buffer) ||
      !flash_.write(flash_.context, target, work->header, HEADER_SIZE) ||
      !flash_.read(flash_.context, target, work->buffer, HEADER_SIZE) ||
      std::memcmp(work->header, work->buffer, HEADER_SIZE))
    return Result::Io;
  uint8_t marker[4];
  put32(marker, COMMITTED);
  if (!flash_.write(flash_.context, target + 24, marker, sizeof(marker)) ||
      !flash_.read(flash_.context, target + 24, work->buffer, sizeof(marker)) ||
      std::memcmp(marker, work->buffer, sizeof(marker)))
    return Result::Io;
  pending_ = true;
  return Result::Ok;
}
}  // namespace hyphenation_pack
