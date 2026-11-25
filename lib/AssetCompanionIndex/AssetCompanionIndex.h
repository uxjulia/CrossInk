#pragma once

#include <AssetCatalog.h>

#include <cstddef>
#include <cstdint>

namespace AssetCompanionIndex {
constexpr size_t MAX_INDEX_BYTES = 2097152;
constexpr uint32_t MAX_FILE_BYTES = 2097152;
constexpr size_t MAX_REFERENCES_PER_INDEX = 16;
constexpr size_t MAX_REFERENCES = 32;
constexpr uint32_t MAX_REFERENCED_BYTES = 8388608;
constexpr size_t MAX_PATH = 240;
struct Reference {
  char path[MAX_PATH + 1] = {};
  char sha256[65] = {};
  uint32_t bytes = 0;
};
struct Options {
  AssetCatalog::Kind kind;
  const char* code;
  const char* revision;
  bool (*onReference)(const Reference&, void*);
  void* context;
};

// No allocations. Keep this object off small task stacks; reuse one per index.
// Options' strings must outlive the parser. Callback references live only until
// the callback returns. All emitted data is untrusted until finish() succeeds.
class Parser {
 public:
  explicit Parser(const Options& options);
  bool feed(const char* bytes, size_t size);
  bool finish();
  const char* error() const { return error_; }
  size_t receivedBytes() const { return received_; }

 private:
  enum class Role : uint8_t { Root, SelectedMap, OtherMap, Entry, Files, Reference, Object, Array };
  enum class State : uint8_t { KeyOrEnd, Key, Colon, Value, ValueOrEnd, CommaOrEnd };
  enum class Field : uint8_t { Ignore, SelectedKind, OtherKind, SelectedCode, Files, Path, Sha256, Bytes };
  enum class Lex : uint8_t { None, String, Escape, Unicode, LowSlash, LowU, Number, Literal };
  struct Frame {
    Role role;
    State state;
    Field field;
    uint8_t seen;
  };
  static constexpr size_t MAX_DEPTH = 16;
  static constexpr size_t MAX_KEY_BYTES = 64;
  static constexpr size_t MAX_STRING_BYTES = 65536;
  static constexpr size_t MAX_TOKENS = 131072;
  Options options_;
  Reference reference_;
  Frame frames_[MAX_DEPTH] = {};
  char token_[MAX_PATH + 1] = {};
  const char* error_ = nullptr;
  size_t received_ = 0;
  uint32_t tokens_ = 0;
  uint32_t stringBytes_ = 0;
  uint32_t referencedBytes_ = 0;
  uint32_t utf8Value_ = 0;
  uint32_t utf8Minimum_ = 0;
  uint16_t tokenSize_ = 0;
  uint16_t unicode_ = 0;
  uint16_t highSurrogate_ = 0;
  uint8_t unicodeDigits_ = 0;
  uint8_t utf8Remaining_ = 0;
  uint8_t depth_ = 0;
  uint8_t referenceCount_ = 0;
  Lex lex_ = Lex::None;
  bool key_ = false;
  bool capture_ = false;
  bool selectedSeen_ = false;
  bool started_ = false;
  bool done_ = false;
  bool finished_ = false;
  bool fail(const char* reason);
  bool consume(uint8_t ch);
  bool punctuation(uint8_t ch);
  bool beginValue(uint8_t ch);
  bool push(Role role);
  bool close(uint8_t ch);
  bool valueComplete();
  bool keyComplete();
  bool stringComplete();
  bool primitiveComplete();
  bool append(uint8_t ch);
  bool appendCodepoint(uint32_t cp);
};
static_assert(sizeof(Parser) <= 1024, "Companion index parser exceeds its C3 memory budget");
}  // namespace AssetCompanionIndex
