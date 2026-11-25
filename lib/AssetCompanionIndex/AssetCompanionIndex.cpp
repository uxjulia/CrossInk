#include "AssetCompanionIndex.h"

#include <cstring>

namespace AssetCompanionIndex {
namespace {
bool hex(const char* value, size_t size) {
  if (!value || std::strlen(value) != size) return false;
  for (size_t i = 0; i < size; ++i) {
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}
bool code(const char* value) {
  if (!value || !*value || std::strlen(value) > 31) return false;
  for (; *value; ++value) {
    if (!((*value >= 'A' && *value <= 'Z') || (*value >= 'a' && *value <= 'z') || (*value >= '0' && *value <= '9') ||
          *value == '-' || *value == '_'))
      return false;
  }
  return true;
}
bool reserved(const char* segment, size_t length) {
  char stem[5] = {};
  size_t n = 0;
  while (n < length && segment[n] != '.') {
    if (n == 4) return false;
    stem[n] = segment[n] >= 'a' && segment[n] <= 'z' ? segment[n] - ('a' - 'A') : segment[n];
    ++n;
  }
  return std::strcmp(stem, "CON") == 0 || std::strcmp(stem, "PRN") == 0 || std::strcmp(stem, "AUX") == 0 ||
         std::strcmp(stem, "NUL") == 0 ||
         (n == 4 && (std::strncmp(stem, "COM", 3) == 0 || std::strncmp(stem, "LPT", 3) == 0) && stem[3] >= '1' &&
          stem[3] <= '9');
}
bool path(const char* value, const char* revision) {
  if (std::strncmp(value, "snapshots/", 10) != 0 || std::strncmp(value + 10, revision, 40) != 0 ||
      std::strncmp(value + 50, "/source/", 8) != 0 || !value[58])
    return false;
  const char* segment = value;
  for (const char* p = value;; ++p) {
    if (*p == '/' || !*p) {
      const size_t n = p - segment;
      if (!n || segment[n - 1] == '.' || reserved(segment, n)) return false;
      if (!*p) return true;
      segment = p + 1;
    } else if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
                 *p == '_' || *p == '.')) {
      return false;
    }
  }
}
bool whitespace(uint8_t ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }
bool number(const char* s) {
  if (*s == '-') ++s;
  if (*s == '0')
    ++s;
  else {
    if (*s < '1' || *s > '9') return false;
    do {
      ++s;
    } while (*s >= '0' && *s <= '9');
  }
  if (*s == '.') {
    ++s;
    if (*s < '0' || *s > '9') return false;
    do {
      ++s;
    } while (*s >= '0' && *s <= '9');
  }
  if (*s == 'e' || *s == 'E') {
    ++s;
    if (*s == '+' || *s == '-') ++s;
    if (*s < '0' || *s > '9') return false;
    do {
      ++s;
    } while (*s >= '0' && *s <= '9');
  }
  return !*s;
}
}  // namespace

Parser::Parser(const Options& options) : options_(options) {
  if (!code(options.code) || !hex(options.revision, 40) || !options.onReference ||
      (options.kind != AssetCatalog::Kind::Hyphenation && options.kind != AssetCatalog::Kind::Translation)) {
    fail("Invalid parser options");
  }
}
bool Parser::fail(const char* reason) {
  if (!error_) error_ = reason;
  return false;
}
bool Parser::feed(const char* bytes, size_t size) {
  if (error_ || finished_) return fail("Parser already finished");
  if ((!bytes && size) || size > MAX_INDEX_BYTES - received_) return fail("Index exceeds byte limit");
  received_ += size;
  for (size_t i = 0; i < size; ++i)
    if (!consume(static_cast<uint8_t>(bytes[i]))) return false;
  return true;
}
bool Parser::finish() {
  if (error_ || finished_) return fail("Parser already finished");
  finished_ = true;
  if (lex_ != Lex::None || !done_ || !selectedSeen_ || !referenceCount_) return fail("Incomplete selected index");
  return true;
}
bool Parser::push(Role role) {
  if (depth_ == MAX_DEPTH) return fail("Index nesting too deep");
  frames_[depth_++] = {role, role == Role::Files || role == Role::Array ? State::ValueOrEnd : State::KeyOrEnd,
                       Field::Ignore, 0};
  return true;
}
bool Parser::valueComplete() {
  if (!depth_) return fail("Unexpected value");
  frames_[depth_ - 1].state = State::CommaOrEnd;
  return true;
}
bool Parser::close(uint8_t ch) {
  if (!depth_) return fail("Unexpected closing delimiter");
  const Frame& frame = frames_[depth_ - 1];
  const bool array = frame.role == Role::Files || frame.role == Role::Array;
  if ((ch == ']') != array ||
      !(frame.state == State::CommaOrEnd || frame.state == State::KeyOrEnd || frame.state == State::ValueOrEnd))
    return fail("Invalid closing delimiter");
  if (frame.role == Role::Root && !frame.seen) return fail("Missing kind map");
  if (frame.role == Role::Entry && !(frame.seen & 1)) return fail("Missing files");
  if (frame.role == Role::Files && !referenceCount_) return fail("Empty files");
  if (frame.role == Role::Reference) {
    if (frame.seen != 7) return fail("Incomplete reference");
    if (referenceCount_ == MAX_REFERENCES_PER_INDEX || reference_.bytes > MAX_REFERENCED_BYTES - referencedBytes_)
      return fail("Reference budget exceeded");
    ++referenceCount_;
    referencedBytes_ += reference_.bytes;
    if (!options_.onReference(reference_, options_.context)) return fail("Reference callback rejected entry");
  }
  --depth_;
  if (!depth_) {
    done_ = true;
    return true;
  }
  return valueComplete();
}
bool Parser::beginValue(uint8_t ch) {
  Frame& frame = frames_[depth_ - 1];
  Role child = ch == '[' ? Role::Array : Role::Object;
  if (frame.role == Role::Root) {
    if (ch != '{') return fail("Kind must be an object");
    child = frame.field == Field::SelectedKind ? Role::SelectedMap : Role::OtherMap;
  } else if (frame.role == Role::SelectedMap && frame.field == Field::SelectedCode) {
    if (ch != '{') return fail("Selected entry must be an object");
    child = Role::Entry;
  } else if (frame.role == Role::SelectedMap || frame.role == Role::OtherMap) {
    if (ch != '{') return fail("Entry must be an object");
  } else if (frame.role == Role::Entry && frame.field == Field::Files) {
    if (ch != '[') return fail("Files must be an array");
    child = Role::Files;
  } else if (frame.role == Role::Files) {
    if (ch != '{') return fail("File reference must be an object");
    child = Role::Reference;
    reference_ = Reference{};
  } else if (frame.role == Role::Reference) {
    if (frame.field == Field::Bytes ? !(ch >= '0' && ch <= '9') : ch != '"')
      return fail("Incorrect reference field type");
  }
  if (ch == '{' || ch == '[') return push(child);
  tokenSize_ = 0;
  token_[0] = 0;
  key_ = false;
  capture_ = frame.role == Role::Reference;
  stringBytes_ = 0;
  if (ch == '"') {
    lex_ = Lex::String;
    return true;
  }
  if (ch == '-' || (ch >= '0' && ch <= '9'))
    lex_ = Lex::Number;
  else if (ch == 't' || ch == 'f' || ch == 'n')
    lex_ = Lex::Literal;
  else
    return fail("Invalid JSON value");
  token_[tokenSize_++] = ch;
  return true;
}
bool Parser::punctuation(uint8_t ch) {
  if (whitespace(ch)) return true;
  if (done_) return fail("Trailing data");
  if (++tokens_ > MAX_TOKENS) return fail("Too many JSON tokens");
  if (!started_) {
    started_ = true;
    return ch == '{' ? push(Role::Root) : fail("Index must be an object");
  }
  Frame& frame = frames_[depth_ - 1];
  if (ch == '}' || ch == ']') return close(ch);
  switch (frame.state) {
    case State::KeyOrEnd:
    case State::Key:
      if (ch != '"') return fail("Expected object key");
      key_ = capture_ = true;
      tokenSize_ = 0;
      stringBytes_ = 0;
      lex_ = Lex::String;
      return true;
    case State::Colon:
      if (ch != ':') return fail("Expected colon");
      frame.state = State::Value;
      return true;
    case State::ValueOrEnd:
    case State::Value:
      return beginValue(ch);
    case State::CommaOrEnd:
      if (ch != ',') return fail("Expected comma");
      frame.state = frame.role == Role::Files || frame.role == Role::Array ? State::Value : State::Key;
      return true;
  }
  return fail("Invalid parser state");
}
bool Parser::keyComplete() {
  Frame& frame = frames_[depth_ - 1];
  frame.field = Field::Ignore;
  uint8_t mask = 0;
  if (frame.role == Role::Root) {
    if (std::strcmp(token_, "hyphenation") == 0)
      mask = 1;
    else if (std::strcmp(token_, "translation") == 0)
      mask = 2;
    else
      return fail("Unknown index kind");
    const bool selected = (mask == 1) == (options_.kind == AssetCatalog::Kind::Hyphenation);
    frame.field = selected ? Field::SelectedKind : Field::OtherKind;
  } else if (frame.role == Role::SelectedMap || frame.role == Role::OtherMap) {
    if (!code(token_)) return fail("Invalid code");
    if (frame.role == Role::SelectedMap && std::strcmp(token_, options_.code) == 0) {
      if (selectedSeen_) return fail("Duplicate selected code");
      selectedSeen_ = true;
      frame.field = Field::SelectedCode;
    }
  } else if (frame.role == Role::Entry && std::strcmp(token_, "files") == 0) {
    frame.field = Field::Files;
    mask = 1;
  } else if (frame.role == Role::Reference) {
    if (std::strcmp(token_, "path") == 0) {
      frame.field = Field::Path;
      mask = 1;
    } else if (std::strcmp(token_, "sha256") == 0) {
      frame.field = Field::Sha256;
      mask = 2;
    } else if (std::strcmp(token_, "bytes") == 0) {
      frame.field = Field::Bytes;
      mask = 4;
    } else
      return fail("Unknown reference field");
  }
  if (frame.seen & mask) return fail("Duplicate required field");
  frame.seen |= mask;
  frame.state = State::Colon;
  return true;
}
bool Parser::stringComplete() {
  token_[tokenSize_] = 0;
  if (key_) return keyComplete();
  Frame& frame = frames_[depth_ - 1];
  if (frame.role == Role::Reference) {
    if (frame.field == Field::Path) {
      if (tokenSize_ < 59 || !path(token_, options_.revision)) return fail("Unsafe reference path");
      std::memcpy(reference_.path, token_, tokenSize_ + 1);
    } else if (frame.field == Field::Sha256) {
      if (!hex(token_, 64)) return fail("Invalid SHA-256");
      std::memcpy(reference_.sha256, token_, 65);
    } else
      return fail("Invalid reference string");
  }
  return valueComplete();
}
bool Parser::primitiveComplete() {
  token_[tokenSize_] = 0;
  const bool numeric = lex_ == Lex::Number;
  if (numeric ? !number(token_)
              : (std::strcmp(token_, "true") && std::strcmp(token_, "false") && std::strcmp(token_, "null")))
    return fail("Invalid JSON primitive");
  const Frame& frame = frames_[depth_ - 1];
  if (frame.role == Role::Reference) {
    if (frame.field != Field::Bytes || !numeric) return fail("Invalid reference primitive");
    uint32_t size = 0;
    for (size_t i = 0; i < tokenSize_; ++i) {
      if (token_[i] < '0' || token_[i] > '9' || size > (MAX_FILE_BYTES - (token_[i] - '0')) / 10)
        return fail("Invalid reference size");
      size = size * 10 + token_[i] - '0';
    }
    if (!size) return fail("Empty reference");
    reference_.bytes = size;
  }
  return valueComplete();
}
bool Parser::append(uint8_t ch) {
  if (++stringBytes_ > MAX_STRING_BYTES) return fail("String exceeds byte limit");
  if (key_ && (!ch || stringBytes_ > MAX_KEY_BYTES)) return fail("Invalid or oversized key");
  if (capture_) {
    if (!ch || tokenSize_ == MAX_PATH) return fail("Captured string exceeds byte limit");
    token_[tokenSize_++] = ch;
  }
  return true;
}
bool Parser::appendCodepoint(uint32_t cp) {
  if (cp < 0x80) return append(cp);
  if (cp < 0x800) return append(0xc0 | (cp >> 6)) && append(0x80 | (cp & 63));
  if (cp < 0x10000) return append(0xe0 | (cp >> 12)) && append(0x80 | ((cp >> 6) & 63)) && append(0x80 | (cp & 63));
  return append(0xf0 | (cp >> 18)) && append(0x80 | ((cp >> 12) & 63)) && append(0x80 | ((cp >> 6) & 63)) &&
         append(0x80 | (cp & 63));
}
bool Parser::consume(uint8_t ch) {
  if (lex_ == Lex::None) return punctuation(ch);
  if (lex_ == Lex::Number || lex_ == Lex::Literal) {
    if (whitespace(ch) || ch == ',' || ch == ']' || ch == '}') {
      if (!primitiveComplete()) return false;
      lex_ = Lex::None;
      return punctuation(ch);
    }
    if (tokenSize_ == 64) return fail("Primitive exceeds byte limit");
    token_[tokenSize_++] = ch;
    return true;
  }
  if (lex_ == Lex::LowSlash) {
    if (ch != '\\') return fail("Missing low surrogate");
    lex_ = Lex::LowU;
    return true;
  }
  if (lex_ == Lex::LowU) {
    if (ch != 'u') return fail("Missing low surrogate");
    unicode_ = unicodeDigits_ = 0;
    lex_ = Lex::Unicode;
    return true;
  }
  if (lex_ == Lex::Unicode) {
    const int digit = ch >= '0' && ch <= '9'   ? ch - '0'
                      : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                      : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                               : -1;
    if (digit < 0) return fail("Invalid Unicode escape");
    unicode_ = (unicode_ << 4) | digit;
    if (++unicodeDigits_ < 4) return true;
    if (highSurrogate_) {
      if (unicode_ < 0xdc00 || unicode_ > 0xdfff) return fail("Invalid low surrogate");
      const uint32_t cp = 0x10000 + ((highSurrogate_ - 0xd800) << 10) + unicode_ - 0xdc00;
      highSurrogate_ = 0;
      lex_ = Lex::String;
      return appendCodepoint(cp);
    }
    if (unicode_ >= 0xd800 && unicode_ <= 0xdbff) {
      highSurrogate_ = unicode_;
      lex_ = Lex::LowSlash;
      return true;
    }
    if (unicode_ >= 0xdc00 && unicode_ <= 0xdfff) return fail("Unpaired low surrogate");
    lex_ = Lex::String;
    return appendCodepoint(unicode_);
  }
  if (lex_ == Lex::Escape) {
    lex_ = Lex::String;
    switch (ch) {
      case '"':
      case '\\':
      case '/':
        return append(ch);
      case 'b':
        return append('\b');
      case 'f':
        return append('\f');
      case 'n':
        return append('\n');
      case 'r':
        return append('\r');
      case 't':
        return append('\t');
      case 'u':
        unicode_ = unicodeDigits_ = 0;
        lex_ = Lex::Unicode;
        return true;
      default:
        return fail("Invalid string escape");
    }
  }
  if (utf8Remaining_) {
    if ((ch & 0xc0) != 0x80) return fail("Invalid UTF-8 continuation");
    utf8Value_ = (utf8Value_ << 6) | (ch & 63);
    if (!--utf8Remaining_ &&
        (utf8Value_ < utf8Minimum_ || utf8Value_ > 0x10ffff || (utf8Value_ >= 0xd800 && utf8Value_ <= 0xdfff)))
      return fail("Invalid UTF-8 codepoint");
    return append(ch);
  }
  if (ch == '"') {
    lex_ = Lex::None;
    return stringComplete();
  }
  if (ch == '\\') {
    lex_ = Lex::Escape;
    return true;
  }
  if (ch < 0x20) return fail("Unescaped string control");
  if (ch >= 0x80) {
    if (ch >= 0xc2 && ch <= 0xdf) {
      utf8Remaining_ = 1;
      utf8Value_ = ch & 31;
      utf8Minimum_ = 0x80;
    } else if (ch >= 0xe0 && ch <= 0xef) {
      utf8Remaining_ = 2;
      utf8Value_ = ch & 15;
      utf8Minimum_ = 0x800;
    } else if (ch >= 0xf0 && ch <= 0xf4) {
      utf8Remaining_ = 3;
      utf8Value_ = ch & 7;
      utf8Minimum_ = 0x10000;
    } else
      return fail("Invalid UTF-8 lead byte");
  }
  return append(ch);
}
}  // namespace AssetCompanionIndex
