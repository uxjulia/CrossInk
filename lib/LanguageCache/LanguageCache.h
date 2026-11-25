#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>

namespace language_cache {
constexpr size_t SLOT_SIZE = 65536;
constexpr size_t HEADER_SIZE = 256;
constexpr size_t OWNER_SIZE = 16;
constexpr size_t CODE_SIZE = 32;
constexpr size_t NAME_SIZE = 96;
constexpr size_t MAX_KEYS = 2048;
constexpr uint16_t MISSING = 0xffff;
constexpr size_t MAX_SOURCE_SIZE = 256 * 1024;
constexpr char DIRECTORY[] = "/.crossink/languages";

// Finder copies these metadata companions alongside SD files. They are not payloads.
constexpr bool isAppleDoubleFile(const char* filename) { return filename && filename[0] == '.' && filename[1] == '_'; }

struct Metadata {
  char code[CODE_SIZE] = {};
  char name[NAME_SIZE] = {};
  char keyboard[CODE_SIZE] = {};
  bool rtl = false;
};

struct Key {
  uint64_t hash;
  uint64_t englishHash;
  const char* name;
  uint16_t id;
  bool formatted;
};

// All callbacks run on cold paths; no ownership is transferred.
struct Schema {
  size_t count;
  Key (*at)(size_t);
  const char* (*english)(uint16_t);
};
struct Input {
  void* context;
  int (*read)(void*, void*, size_t);  // zero = EOF, negative = error
  bool (*rewind)(void*) = nullptr;    // same open source; needed for changed pinned-slot installs
};
// The caller supplies firmware-owned storage. Installation may replace existing
// data in its final two slots; the rest of the region is never modified.
struct Flash {
  void* context;
  size_t size;
  bool (*read)(void*, size_t, void*, size_t);
  bool (*write)(void*, size_t, const void*, size_t);
  bool (*erase)(void*, size_t, size_t);
  // Optional bounded readback with one storage handle for bulk writes.
  bool (*verify)(void*, size_t, const void*, size_t) = nullptr;
};

enum class Result : uint8_t {
  Ok,
  Io,
  Memory,
  Invalid,
  TooLarge,
  Duplicate,
  MetadataMissing,
  StorageUnavailable,
  Cancelled,
  RestartRequired,
  Unchanged
};
const char* resultName(Result result);
uint64_t hash(const char* text);
bool validUtf8(const char* text, size_t length);
bool validCode(const char* code);
bool compatibleFormat(const char* english, const char* translated);

// Reasons point to static text; no source strings or extra allocation are retained.
struct InspectionDiagnostic {
  const char* reason = nullptr;
  // One-based line; offset counts consumed bytes through that line or read failure.
  size_t line = 0;
  size_t offset = 0;
};
// Reuse a single bounded workspace while enumerating a language directory.
class Inspector {
 public:
  Inspector();
  ~Inspector();
  bool available() const { return bool(workspace_); }
  Result inspect(Input input, Metadata& metadata, InspectionDiagnostic* diagnostic = nullptr);

 private:
  struct Impl;
  HeapObject<Impl> workspace_;
};

// Inspect metadata without reading beyond the first string. Metadata must precede strings.
Result inspect(Input input, Metadata& metadata, InspectionDiagnostic* diagnostic = nullptr);

// Optional online admission checks. Null expectations preserve manual-source semantics.
// continueWork may yield and returns false to cancel; called at most every 256 input bytes.
struct ValidationOptions {
  const char* expectedCode = nullptr;
  const char* sourceSha256 = nullptr;
  const char* keysetSha256 = nullptr;
  void* context = nullptr;
  bool (*continueWork)(void*) = nullptr;
};
// Reads the entire source and checks compiled slot feasibility. No flash API is
// accepted: validation cannot install or change the active language.
Result validate(Input input, const Schema& schema, Metadata& metadata, const ValidationOptions& options = {});

struct Installed {
  Metadata metadata;
  uint64_t generation = 0;
  uint32_t checksum = 0;
  int slot = -1;
};

// The caller pins the selected slot until restart. Installation never changes it.
// Identical compiled content (including record order) and metadata returns
// Unchanged with the pinned identity
// and no writes. A changed source requires Input::rewind before any flash mutation.
// Optional caller-owned SLOT_SIZE scratch batches the inactive slot in RAM.
// Storage is still verified before the commit marker is published last.
Result install(Input input, const Schema& schema, const Flash& flash, int pinnedSlot, Installed& installed,
               uint8_t* staging = nullptr);

// Fully validates a committed mapped slot. Offsets are relative to the slot base.
// Pass nullptr for offsets to inspect a candidate without changing the active index.
bool open(const uint8_t* bytes, size_t length, const Schema& schema, Installed& installed, uint16_t* offsets = nullptr);
size_t slotOffset(size_t partitionSize, int slot);
}  // namespace language_cache
