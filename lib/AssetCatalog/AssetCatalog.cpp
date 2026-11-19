#include "AssetCatalog.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace AssetCatalog {
namespace {
bool isHex(const char* value, size_t size) {
  if (std::strlen(value) != size) return false;
  for (size_t i = 0; i < size; ++i) {
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}
bool letter(char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
char lower(char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }
bool sameCode(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) {
    ++a;
    ++b;
  }
  return *a == *b;
}
bool validCode(const char* code) {
  const size_t size = std::strlen(code);
  if (size == 0 || size > 31 || !letter(code[0])) return false;
  for (size_t i = 1; i < size; ++i) {
    if (!letter(code[i]) && !(code[i] >= '0' && code[i] <= '9') && code[i] != '-' && code[i] != '_') return false;
  }
  return true;
}
bool hyphenationCode(const char* code) {
  static constexpr const char* codes[] = {"fr", "de", "ru", "es", "it", "pl", "pt", "sv", "uk"};
  for (const auto* supported : codes)
    if (std::strcmp(code, supported) == 0) return true;
  return false;
}
}  // namespace

bool catalogPath(const Options& options, char* output, size_t capacity) {
  if (!output || !capacity) return false;
  output[0] = '\0';
  const bool translation = options.expectedKind == Kind::Translation;
  if (translation && (!options.expectedSource || !options.expectedKeyset || !isHex(options.expectedSource, 64) ||
                      !isHex(options.expectedKeyset, 64)))
    return false;
  const char* channel = options.expectedChannel == Channel::Rc ? "rc" : "stable";
  const int size = translation ? std::snprintf(output, capacity, "catalogs/%s/translation/%s/%s.json", channel,
                                               options.expectedSource, options.expectedKeyset)
                               : std::snprintf(output, capacity, "catalogs/%s/hyphenation.json", channel);
  if (size < 0 || static_cast<size_t>(size) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

Parser::Parser(Catalog& catalog, const Options& options) : catalog_(catalog), options_(options) {
  // Reset in place; avoid a large temporary on the constrained device stack.
  catalog_.count = 0;
  catalog_.revision[0] = catalog_.source[0] = catalog_.keyset[0] = '\0';
  catalog_.kind = options.expectedKind;
  catalog_.channel = options.expectedChannel;
  if (options_.expectedKind == Kind::Translation &&
      (!options_.expectedSource || !options_.expectedKeyset || !isHex(options_.expectedSource, 64) ||
       !isHex(options_.expectedKeyset, 64) || !options_.supportedTranslation))
    fail("Missing translation contract");
}
bool Parser::fail(const char* reason) {
  if (!error_) error_ = reason;
  return false;
}
bool Parser::feed(const char* bytes, size_t size) {
  if (error_) return false;
  if (finished_) return fail("Feed after finish");
  if (size > MAX_CATALOG_BYTES - received_) return fail("Catalog exceeds byte limit");
  if (size && !bytes) return fail("Null input");
  received_ += size;
  for (size_t i = 0; i < size; ++i)
    if (!consume(static_cast<uint8_t>(bytes[i]))) return false;
  return true;
}
bool Parser::finish() {
  if (error_) return false;
  if (lex_ != Lex::None || state_ != State::Done) return fail("Truncated catalog");
  finished_ = true;
  return true;
}
bool Parser::append(uint8_t ch) {
  if (tokenSize_ >= sizeof(token_) - 1) return fail("String exceeds byte limit");
  if (utf8Remaining_) {
    if ((ch & 0xc0) != 0x80) return fail("Invalid UTF-8 continuation");
    utf8Value_ = (utf8Value_ << 6) | (ch & 0x3f);
    if (--utf8Remaining_ == 0 &&
        (utf8Value_ < utf8Minimum_ || utf8Value_ > 0x10ffff || (utf8Value_ >= 0xd800 && utf8Value_ <= 0xdfff) ||
         (utf8Value_ >= 0x80 && utf8Value_ <= 0x9f)))
      return fail("Invalid UTF-8 scalar");
  } else if (ch < 0x80) {
    if (ch < 0x20 || ch == 0x7f) return fail("Control character in string");
  } else if (ch >= 0xc2 && ch <= 0xdf) {
    utf8Value_ = ch & 0x1f;
    utf8Minimum_ = 0x80;
    utf8Remaining_ = 1;
  } else if (ch >= 0xe0 && ch <= 0xef) {
    utf8Value_ = ch & 0xf;
    utf8Minimum_ = 0x800;
    utf8Remaining_ = 2;
  } else if (ch >= 0xf0 && ch <= 0xf4) {
    utf8Value_ = ch & 7;
    utf8Minimum_ = 0x10000;
    utf8Remaining_ = 3;
  } else
    return fail("Invalid UTF-8 lead byte");
  token_[tokenSize_++] = static_cast<char>(ch);
  token_[tokenSize_] = '\0';
  return true;
}
bool Parser::appendCodepoint(uint32_t cp) {
  if (cp < 0x80) return append(cp);
  if (cp < 0x800) return append(0xc0 | (cp >> 6)) && append(0x80 | (cp & 0x3f));
  if (cp < 0x10000) return append(0xe0 | (cp >> 12)) && append(0x80 | ((cp >> 6) & 0x3f)) && append(0x80 | (cp & 0x3f));
  return append(0xf0 | (cp >> 18)) && append(0x80 | ((cp >> 12) & 0x3f)) && append(0x80 | ((cp >> 6) & 0x3f)) &&
         append(0x80 | (cp & 0x3f));
}
bool Parser::consume(uint8_t ch) {
  if (lex_ == Lex::Number) {
    if (ch >= '0' && ch <= '9') {
      if (zeroNumber_ || number_ > (std::numeric_limits<uint32_t>::max() - (ch - '0')) / 10)
        return fail("Invalid integer");
      number_ = number_ * 10 + ch - '0';
      return true;
    }
    lex_ = Lex::None;
    if (!numberComplete()) return false;
    return punctuation(ch);
  }
  if (lex_ == Lex::LowSlash) {
    if (ch != '\\') return fail("Missing low surrogate");
    lex_ = Lex::LowU;
    return true;
  }
  if (lex_ == Lex::LowU) {
    if (ch != 'u') return fail("Missing low surrogate");
    lex_ = Lex::Unicode;
    unicode_ = 0;
    unicodeDigits_ = 0;
    return true;
  }
  if (lex_ == Lex::Unicode) {
    uint8_t digit;
    if (ch >= '0' && ch <= '9')
      digit = ch - '0';
    else if (ch >= 'a' && ch <= 'f')
      digit = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F')
      digit = ch - 'A' + 10;
    else
      return fail("Invalid Unicode escape");
    unicode_ = (unicode_ << 4) | digit;
    if (++unicodeDigits_ != 4) return true;
    lex_ = Lex::String;
    if (highSurrogate_) {
      if (unicode_ < 0xdc00 || unicode_ > 0xdfff) return fail("Invalid low surrogate");
      const uint32_t cp = 0x10000 + ((highSurrogate_ - 0xd800) << 10) + unicode_ - 0xdc00;
      highSurrogate_ = 0;
      return appendCodepoint(cp);
    }
    if (unicode_ >= 0xd800 && unicode_ <= 0xdbff) {
      highSurrogate_ = unicode_;
      lex_ = Lex::LowSlash;
      return true;
    }
    if (unicode_ >= 0xdc00 && unicode_ <= 0xdfff) return fail("Unpaired low surrogate");
    return appendCodepoint(unicode_);
  }
  if (lex_ == Lex::Escape) {
    lex_ = Lex::String;
    if (ch == '"' || ch == '\\' || ch == '/') return append(ch);
    if (ch == 'u') {
      unicode_ = 0;
      unicodeDigits_ = 0;
      lex_ = Lex::Unicode;
      return true;
    }
    // Other JSON escapes are control characters, forbidden by every v1 field.
    return fail("Invalid or control escape");
  }
  if (lex_ == Lex::String) {
    if (ch == '"' || ch == '\\') {
      if (utf8Remaining_) return fail("Truncated UTF-8");
      if (ch == '\\') {
        lex_ = Lex::Escape;
        return true;
      }
      lex_ = Lex::None;
      return stringComplete();
    }
    return append(ch);
  }
  return punctuation(ch);
}
bool Parser::punctuation(uint8_t ch) {
  if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') return true;
  if (ch == '"') {
    switch (state_) {
      case State::RootKeyOrEnd:
      case State::RootKey:
      case State::EntryKeyOrEnd:
      case State::EntryKey:
      case State::RootValue:
      case State::EntryValue:
        tokenSize_ = 0;
        token_[0] = '\0';
        lex_ = Lex::String;
        return true;
      default:
        return fail("Unexpected string");
    }
  }
  if (ch >= '0' && ch <= '9' && (state_ == State::RootValue || state_ == State::EntryValue)) {
    number_ = ch - '0';
    zeroNumber_ = ch == '0';
    lex_ = Lex::Number;
    return true;
  }
  switch (state_) {
    case State::Start:
      if (ch == '{') {
        state_ = State::RootKeyOrEnd;
        return true;
      }
      break;
    case State::RootKeyOrEnd:
      if (ch == '}') return endRoot();
      break;
    case State::RootColon:
      if (ch == ':') {
        state_ = State::RootValue;
        return true;
      }
      break;
    case State::RootValue:
      if (field_ == Entries && ch == '[') {
        state_ = State::EntryOrEnd;
        return true;
      }
      break;
    case State::RootCommaOrEnd:
      if (ch == '}') return endRoot();
      if (ch == ',') {
        state_ = State::RootKey;
        return true;
      }
      break;
    case State::EntryOrEnd:
      if (ch == ']') {
        state_ = State::RootCommaOrEnd;
        return true;
      }
      if (ch == '{') return beginEntry();
      break;
    case State::Entry:
      if (ch == '{') return beginEntry();
      break;
    case State::ArrayCommaOrEnd:
      if (ch == ']') {
        state_ = State::RootCommaOrEnd;
        return true;
      }
      if (ch == ',') {
        state_ = State::Entry;
        return true;
      }
      break;
    case State::EntryKeyOrEnd:
      if (ch == '}') return endEntry();
      break;
    case State::EntryColon:
      if (ch == ':') {
        state_ = State::EntryValue;
        return true;
      }
      break;
    case State::EntryCommaOrEnd:
      if (ch == '}') return endEntry();
      if (ch == ',') {
        state_ = State::EntryKey;
        return true;
      }
      break;
    default:
      break;
  }
  return fail("Invalid JSON syntax or type");
}
bool Parser::keyComplete(bool entry) {
  if (tokenSize_ > 31) return fail("Field name exceeds byte limit");
  static constexpr const char* fields[] = {"schema",  "kind",    "channel",  "revision", "format", "requires",
                                           "notices", "sources", "entries",  "source",   "keyset", "code",
                                           "bytes",   "sha256",  "previous", "name"};
  field_ = Invalid;
  for (uint8_t i = entry ? Code : Schema; i < (entry ? Invalid : Code); ++i) {
    if (std::strcmp(token_, fields[i]) == 0) {
      field_ = static_cast<Field>(i);
      break;
    }
  }
  if (field_ == Invalid) return fail("Unknown field");
  if (options_.expectedKind == Kind::Hyphenation && (field_ == Source || field_ == Keyset || field_ == Name))
    return fail("Translation field in hyphenation catalog");
  uint32_t& fieldsSeen = entry ? entryFields_ : rootFields_;
  const uint32_t bit = 1u << field_;
  if (fieldsSeen & bit) return fail("Duplicate field");
  fieldsSeen |= bit;
  state_ = entry ? State::EntryColon : State::RootColon;
  return true;
}
bool Parser::stringComplete() {
  if (state_ == State::RootKeyOrEnd || state_ == State::RootKey) return keyComplete(false);
  if (state_ == State::EntryKeyOrEnd || state_ == State::EntryKey) return keyComplete(true);
  char* destination = nullptr;
  switch (field_) {
    case KindField:
      if (std::strcmp(token_, options_.expectedKind == Kind::Hyphenation ? "hyphenation" : "translation") != 0)
        return fail("Wrong catalog kind");
      break;
    case ChannelField:
      if (std::strcmp(token_, options_.expectedChannel == Channel::Stable ? "stable" : "rc") != 0)
        return fail("Wrong catalog channel");
      break;
    case Revision:
      if (!isHex(token_, 40)) return fail("Invalid revision");
      destination = catalog_.revision;
      break;
    case Notices:
      if (std::strcmp(token_, "notices.json") != 0) return fail("Invalid notices path");
      break;
    case Sources:
      if (std::strcmp(token_, "sources.json") != 0) return fail("Invalid sources path");
      break;
    case Source:
      if (std::strcmp(token_, options_.expectedSource) != 0) return fail("Source contract mismatch");
      destination = catalog_.source;
      break;
    case Keyset:
      if (std::strcmp(token_, options_.expectedKeyset) != 0) return fail("Keyset contract mismatch");
      destination = catalog_.keyset;
      break;
    case Code:
      if (!validCode(token_) || sameCode(token_, "en")) return fail("Invalid or built-in code");
      if (options_.expectedKind == Kind::Hyphenation && !hyphenationCode(token_))
        return fail("Unsupported language code");
      destination = catalog_.entries[catalog_.count].code;
      break;
    case Sha256:
    case Previous:
      if (!isHex(token_, 64)) return fail("Invalid SHA-256");
      destination =
          field_ == Sha256 ? catalog_.entries[catalog_.count].sha256 : catalog_.entries[catalog_.count].previous;
      break;
    case Name:
      if (!tokenSize_) return fail("Empty name");
      destination = catalog_.entries[catalog_.count].name;
      break;
    default:
      return fail("Unexpected string type");
  }
  if (destination) std::memcpy(destination, token_, tokenSize_ + 1);
  state_ = state_ == State::RootValue ? State::RootCommaOrEnd : State::EntryCommaOrEnd;
  return true;
}
bool Parser::numberComplete() {
  switch (field_) {
    case Schema:
    case Format:
      if (number_ != 1) return fail("Unsupported schema or format");
      break;
    case Requires:
      if (number_ != 0) return fail("Unsupported capabilities");
      break;
    case Bytes:
      if (number_ == 0 || number_ > MAX_ASSET_BYTES) return fail("Invalid asset size");
      catalog_.entries[catalog_.count].bytes = number_;
      break;
    default:
      return fail("Unexpected integer type");
  }
  state_ = state_ == State::RootValue ? State::RootCommaOrEnd : State::EntryCommaOrEnd;
  return true;
}
bool Parser::beginEntry() {
  const size_t limit = options_.expectedKind == Kind::Hyphenation ? MAX_HYPHENATION_ENTRIES : MAX_ENTRIES;
  if (catalog_.count >= limit) return fail("Too many entries");
  auto& entry = catalog_.entries[catalog_.count];
  entry.code[0] = entry.name[0] = entry.sha256[0] = entry.previous[0] = '\0';
  entry.bytes = 0;
  entryFields_ = 0;
  state_ = State::EntryKeyOrEnd;
  return true;
}
bool Parser::endEntry() {
  uint32_t required = (1u << Code) | (1u << Bytes) | (1u << Sha256);
  if (options_.expectedKind == Kind::Translation) required |= 1u << Name;
  if ((entryFields_ & required) != required) return fail("Missing entry field");
  const auto& entry = catalog_.entries[catalog_.count];
  if (std::strcmp(entry.sha256, entry.previous) == 0) return fail("Previous equals current hash");
  for (size_t i = 0; i < catalog_.count; ++i) {
    if (sameCode(catalog_.entries[i].code, entry.code)) return fail("Duplicate or case-colliding code");
  }
  ++catalog_.count;
  state_ = State::ArrayCommaOrEnd;
  return true;
}
bool Parser::endRoot() {
  uint32_t required = (1u << (Entries + 1)) - 1;
  if (options_.expectedKind == Kind::Translation) required |= (1u << Source) | (1u << Keyset);
  if ((rootFields_ & required) != required) return fail("Missing root field");
  if (options_.expectedKind == Kind::Translation) {
    // Validate and bound every entry before filtering. Future language additions
    // must not block downloads for a firmware's existing registry.
    size_t retained = 0;
    for (size_t i = 0; i < catalog_.count; ++i) {
      if (!options_.supportedTranslation(catalog_.entries[i].code, options_.context)) continue;
      if (retained != i) catalog_.entries[retained] = catalog_.entries[i];
      ++retained;
    }
    catalog_.count = retained;
  }
  state_ = State::Done;
  return true;
}
bool immutableAssetPath(const Metadata& catalog, const Entry& entry, char* output, size_t capacity) {
  if (!output || !capacity) return false;
  const bool hyphenation = catalog.kind == Kind::Hyphenation;
  const int size =
      std::snprintf(output, capacity, "snapshots/%s/%s/%s.%s", catalog.revision,
                    hyphenation ? "hyphenation" : "translation", entry.sha256, hyphenation ? "cphyph" : "yaml");
  if (size < 0 || static_cast<size_t>(size) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}
SourceStatus classifySource(const Entry& entry, const char* actualSha256) {
  if (!actualSha256 || !isHex(actualSha256, 64)) return SourceStatus::Different;
  if (std::strcmp(entry.sha256, actualSha256) == 0) return SourceStatus::Current;
  if (entry.previous[0] && std::strcmp(entry.previous, actualSha256) == 0) return SourceStatus::Previous;
  return SourceStatus::Different;
}
}  // namespace AssetCatalog
