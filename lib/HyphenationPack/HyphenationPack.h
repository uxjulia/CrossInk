#pragma once

#include <cstddef>
#include <cstdint>

// Portable, bounded storage for upstream CPHY v1 files. The caller supplies
// only the hyphenation region, excluding the UI-language slots and OTA images.
namespace hyphenation_pack {
constexpr size_t HEADER_SIZE = 256;
constexpr size_t MAX_PACKS = 10;
constexpr size_t SECTOR_SIZE = 4096;
constexpr size_t PACK_HEADER_SIZE = 24;
constexpr size_t MAX_SOURCE_SIZE = 256 * 1024;

struct Input {
  void* context;
  size_t size;
  bool (*read)(void*, size_t, void*, size_t);
};
struct Flash {
  void* context;
  size_t size;
  bool (*read)(void*, size_t, void*, size_t);
  bool (*write)(void*, size_t, const void*, size_t);
  bool (*erase)(void*, size_t, size_t);
  void (*yield)();
};
struct Entry {
  char code[3] = {};
  uint8_t minPrefix = 0;
  uint8_t minSuffix = 0;
  uint32_t offset = 0;  // relative to the bank
  uint32_t size = 0;
  uint32_t rootOffset = 0;
  uint32_t checksum = 0;
};
using Supported = bool (*)(const char*, uint8_t, uint8_t);
enum class Result { Ok, Unchanged, Invalid, Unsupported, Io, Memory, NoSpace, Unavailable, RestartRequired, Cancelled };
const char* resultName(Result result);
uint32_t crc32(const void* bytes, size_t length, uint32_t previous = 0);
uint32_t identity(const Entry& entry);

struct ValidationOptions {
  const char* expectedCode = nullptr;
  // Available payload bytes in the destination bank, accounting for other packs.
  size_t maxPayloadSize = MAX_SOURCE_SIZE - PACK_HEADER_SIZE;
  void* context = nullptr;
  // Checked every 64 bounded parse/cache operations; return false to cancel.
  // Keep cancellation cheap and throttle any yielding by elapsed time.
  bool (*continueWork)(void*) = nullptr;
};
// Full read-only CPHY source validation, including CRC and serialized trie
// boundaries. No flash API is accepted and no bank can be mutated.
Result validate(Input input, Supported supported, Entry& entry, const ValidationOptions& options = {});
// Cheap directory discovery only. Full validation is still required for installation.
Result inspect(Input input, Supported supported, const char* expectedCode, Entry& entry);

class Store {
 public:
  // Local-only entries live in a compatible extension invisible to legacy firmware.
  Result begin(Flash flash, Supported legacySupported, Supported localSupported = nullptr,
               size_t localReservedTailBytes = 0);
  Result install(Input input);
  Result remove(const char* code);
  bool find(const char* code, Entry& entry) const;
  bool entryAt(size_t index, Entry& entry) const;
  size_t count() const;
  size_t bankOffset() const { return bank_ < 0 ? 0 : static_cast<size_t>(bank_) * bankSize_; }
  size_t usedBytes() const;
  // Payload capacity after retaining every installed pack except this code.
  // Zero when unavailable, restart is pending, or no entry slot remains.
  size_t availablePayloadBytes(const char* replacementCode) const;
  bool needsRestart() const { return pending_; }

 private:
  // Remains immutable after begin(): consumers can pin this bank until reboot.
  uint8_t header_[HEADER_SIZE] = {};
  Flash flash_{};
  Supported supported_ = nullptr;
  Supported localSupported_ = nullptr;
  size_t bankSize_ = 0;
  size_t localWriteCapacity_ = 0;
  int bank_ = -1;
  bool ready_ = false;
  bool pending_ = false;
  Result rewrite(const char* code, const Input* input, const Entry* replacement);
};
}  // namespace hyphenation_pack
