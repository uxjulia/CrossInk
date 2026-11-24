#pragma once

#include <cstddef>
#include <cstdint>

// Wire contract: docs/asset-catalog-contract-v1.md. Portable, allocation-free.
namespace AssetCatalog {
constexpr size_t MAX_CATALOG_BYTES = 8192;
constexpr size_t MAX_CATALOG_PATH = 192;
constexpr size_t MAX_ENTRIES = 48;
constexpr size_t MAX_LEGACY_HYPHENATION_ENTRIES = 9;
constexpr size_t MAX_HYPHENATION_ENTRIES = 10;
constexpr size_t MAX_SUPPLEMENTAL_HYPHENATION_ENTRIES = 18;
constexpr uint32_t MAX_ASSET_BYTES = 262144;
enum class Kind : uint8_t { Hyphenation, Translation };
enum class Channel : uint8_t { Stable, Rc };
enum class HyphenationCatalog : uint8_t { Legacy = 1, Extended = 2, Supplemental = 3 };
struct Entry {
  char code[32] = {};
  char name[96] = {};
  char sha256[65] = {};
  char previous[65] = {};
  uint32_t bytes = 0;
};
// Shared catalog contract retained after selecting one asset.
struct Metadata {
  Kind kind = Kind::Hyphenation;
  Channel channel = Channel::Stable;
  char revision[41] = {};
  char source[65] = {};
  char keyset[65] = {};
};
struct Selection : Metadata {
  Entry entry;
};
static_assert(sizeof(Selection) <= 512, "Selected asset exceeds C3 metadata budget");
struct Catalog : Metadata {
  Entry entries[MAX_ENTRIES] = {};
  size_t count = 0;
};
struct Options {
  Kind expectedKind = Kind::Hyphenation;
  Channel expectedChannel = Channel::Stable;
  const char* expectedSource = nullptr;
  const char* expectedKeyset = nullptr;
  bool (*supportedTranslation)(const char* code, void* context) = nullptr;
  void* context = nullptr;
  HyphenationCatalog hyphenationCatalog = HyphenationCatalog::Legacy;
  bool (*supportedSupplementalHyphenation)(const char* code) = nullptr;
};

// Translation discovery is scoped to the firmware; hyphenation is scoped to its catalog version.
// A public S3 catalog can return 403 for a missing object without ListBucket.
// Only these absence responses and legacy pack codes allow fallback.
bool canFallbackHyphenationCatalog(const Options& options, int httpStatus, const char* code);
bool catalogPath(const Options& options, char* output, size_t capacity);

// Caller owns catalog and options' pointed-to data for the parser lifetime.
// Allocate the catalog once with makeUniqueNoThrow, never on a device task stack.
// Partial output is untrusted: only consume it after finish() returns true.
class Parser {
 public:
  Parser(Catalog& catalog, const Options& options);
  bool feed(const char* bytes, size_t size);
  bool finish();
  const char* error() const { return error_; }
  size_t receivedBytes() const { return received_; }

 private:
  enum class State : uint8_t {
    Start,
    RootKeyOrEnd,
    RootKey,
    RootColon,
    RootValue,
    RootCommaOrEnd,
    EntryOrEnd,
    Entry,
    ArrayCommaOrEnd,
    EntryKeyOrEnd,
    EntryKey,
    EntryColon,
    EntryValue,
    EntryCommaOrEnd,
    Done
  };
  enum class Lex : uint8_t { None, String, Escape, Unicode, LowSlash, LowU, Number };
  enum Field : uint8_t {
    Schema,
    KindField,
    ChannelField,
    Revision,
    Format,
    Requires,
    Notices,
    Sources,
    Entries,
    Source,
    Keyset,
    Code,
    Bytes,
    Sha256,
    Previous,
    Name,
    Invalid
  };
  Catalog& catalog_;
  Options options_;
  const char* error_ = nullptr;
  size_t received_ = 0;
  State state_ = State::Start;
  Lex lex_ = Lex::None;
  Field field_ = Invalid;
  uint32_t rootFields_ = 0;
  uint32_t entryFields_ = 0;
  char token_[96] = {};
  size_t tokenSize_ = 0;
  uint32_t number_ = 0;
  uint32_t utf8Value_ = 0;
  uint32_t utf8Minimum_ = 0;
  uint16_t unicode_ = 0;
  uint16_t highSurrogate_ = 0;
  uint8_t utf8Remaining_ = 0;
  uint8_t unicodeDigits_ = 0;
  bool zeroNumber_ = false;
  bool finished_ = false;
  bool fail(const char* reason);
  bool consume(uint8_t ch);
  bool punctuation(uint8_t ch);
  bool append(uint8_t ch);
  bool appendCodepoint(uint32_t cp);
  bool stringComplete();
  bool numberComplete();
  bool keyComplete(bool entry);
  bool beginEntry();
  bool endEntry();
  bool endRoot();
};

static_assert(sizeof(Catalog) + sizeof(Parser) < 16384, "Catalog plus parser exceeds C3 memory budget");

// Use only with successfully parsed catalogs. Returns false on short output.
bool immutableAssetPath(const Metadata& catalog, const Entry& entry, char* output, size_t capacity);
enum class SourceStatus : uint8_t { Current, Previous, Different };
SourceStatus classifySource(const Entry& entry, const char* actualSha256);
}  // namespace AssetCatalog
