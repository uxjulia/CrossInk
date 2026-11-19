#include "OpdsAuthChallenge.h"

#include <cstring>

OpdsAuthChallenge::OpdsAuthChallenge()
    : parser(JsonCallbacks{this, key, value, nullptr, nullptr, nullptr, objectStart, objectEnd, arrayStart, arrayEnd}) {
}

bool OpdsAuthChallenge::write(const uint8_t* bytes, size_t length) {
  if ((!bytes && length) || length > 8192 - received) return invalid = true, false;
  received += length;
  // Detect trailing data and truncated roots even though the SDK SAX parser
  // intentionally has no document-finalization API. No full body is retained.
  for (size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(bytes[i]);
    if (rootClosed && c != ' ' && c != '\t' && c != '\r' && c != '\n') invalid = true;
    if (!syntax(c)) invalid = true;
    if (invalid) return false;
    parser.feed(&c, 1);
    if (invalid || parser.hasError()) return false;
  }
  return true;
}
void OpdsAuthChallenge::key(void* ctx, const char* text, size_t length) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  s.pending = length == 14 && !std::memcmp(text, "authentication", 14) ? Key::Authentication
              : length == 4 && !std::memcmp(text, "type", 4)           ? Key::Type
                                                                       : Key::None;
}
void OpdsAuthChallenge::value(void* ctx, const char* text, size_t length) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  constexpr char BASIC[] = "http://opds-spec.org/auth/basic";
  if (s.authObject && s.depth == s.authObject && s.pending == Key::Type && length == sizeof(BASIC) - 1 &&
      !std::memcmp(text, BASIC, length))
    s.basic = true;
  s.pending = Key::None;
}
void OpdsAuthChallenge::objectStart(void* ctx) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  if (!s.depth) {
    if (s.rootSeen) s.invalid = true;
    s.rootSeen = true;
  }
  ++s.depth;
  if (s.authArray && s.depth == s.authArray + 1) s.authObject = s.depth;
  s.pending = Key::None;
}
void OpdsAuthChallenge::objectEnd(void* ctx) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  if (!s.depth) {
    s.invalid = true;
    return;
  }
  if (s.depth == s.authObject) s.authObject = 0;
  if (--s.depth == 0) s.rootClosed = s.rootSeen;
  s.pending = Key::None;
}
void OpdsAuthChallenge::arrayStart(void* ctx) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  if (!s.depth) s.invalid = true;
  ++s.depth;
  if (s.depth == 2 && s.pending == Key::Authentication) s.authArray = s.depth;
  s.pending = Key::None;
}
void OpdsAuthChallenge::arrayEnd(void* ctx) {
  auto& s = *static_cast<OpdsAuthChallenge*>(ctx);
  if (!s.depth) {
    s.invalid = true;
    return;
  }
  if (s.depth == s.authArray) s.authArray = 0;
  --s.depth;
  s.pending = Key::None;
}

bool OpdsAuthChallenge::valueDone() {
  if (!grammarDepth) return false;
  auto& g = grammar[grammarDepth - 1];
  if (g == Grammar::ObjectValue)
    g = Grammar::ObjectComma;
  else if (g == Grammar::ArrayFirst || g == Grammar::ArrayValue)
    g = Grammar::ArrayComma;
  else
    return false;
  return true;
}
bool OpdsAuthChallenge::syntax(char c) {
  const bool whitespace = c == ' ' || c == '\t' || c == '\r' || c == '\n';
  if (token == Token::String) {
    if (hexDigits) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
      --hexDigits;
    } else if (escaped) {
      escaped = false;
      if (c == 'u')
        hexDigits = 4;
      else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' && c != 'n' && c != 'r' && c != 't')
        return false;
    } else if (c == '\\')
      escaped = true;
    else if (c == '"') {
      token = Token::None;
      if (stringKey)
        grammar[grammarDepth - 1] = Grammar::Colon;
      else
        return valueDone();
    } else if (static_cast<unsigned char>(c) < 0x20)
      return false;
    return true;
  }
  if (token == Token::Literal) {
    if (c != *literal++) return false;
    if (!*literal) {
      token = Token::None;
      return valueDone();
    }
    return true;
  }
  if (token == Token::Number) {
    const bool digit = c >= '0' && c <= '9';
    switch (number) {
      case Number::Sign:
        if (!digit) return false;
        number = c == '0' ? Number::Zero : Number::Integer;
        return true;
      case Number::Dot:
        if (!digit) return false;
        number = Number::Fraction;
        return true;
      case Number::Exponent:
        if (c == '+' || c == '-') {
          number = Number::ExponentSign;
          return true;
        }
        if (!digit) return false;
        number = Number::ExponentDigits;
        return true;
      case Number::ExponentSign:
        if (!digit) return false;
        number = Number::ExponentDigits;
        return true;
      default:
        break;
    }
    if (digit) return number != Number::Zero;
    if (c == '.' && (number == Number::Zero || number == Number::Integer)) {
      number = Number::Dot;
      return true;
    }
    if ((c == 'e' || c == 'E') && (number == Number::Zero || number == Number::Integer || number == Number::Fraction)) {
      number = Number::Exponent;
      return true;
    }
    token = Token::None;
    if (!valueDone()) return false;
    // Delimiter belongs to the surrounding object/array and is processed below.
  }
  if (whitespace) return true;
  if (syntaxDone) return false;
  if (!grammarDepth) {
    if (c != '{') return false;
    grammar[grammarDepth++] = Grammar::ObjectFirst;
    return true;
  }
  auto& g = grammar[grammarDepth - 1];
  if (c == '}' && (g == Grammar::ObjectFirst || g == Grammar::ObjectComma)) {
    if (--grammarDepth == 0) syntaxDone = true;
    return true;
  }
  if (c == ']' && (g == Grammar::ArrayFirst || g == Grammar::ArrayComma)) {
    --grammarDepth;
    return grammarDepth != 0;
  }
  if (g == Grammar::Colon) {
    if (c != ':') return false;
    g = Grammar::ObjectValue;
    return true;
  }
  if (g == Grammar::ObjectComma || g == Grammar::ArrayComma) {
    if (c != ',') return false;
    g = g == Grammar::ObjectComma ? Grammar::ObjectKey : Grammar::ArrayValue;
    return true;
  }
  stringKey = g == Grammar::ObjectFirst || g == Grammar::ObjectKey;
  if (stringKey && c != '"') return false;
  if (c == '"') {
    token = Token::String;
    return true;
  }
  if (c == '{' || c == '[') {
    if (!valueDone() || grammarDepth >= StreamingJsonParser::MAX_NESTING) return false;
    grammar[grammarDepth++] = c == '{' ? Grammar::ObjectFirst : Grammar::ArrayFirst;
    return true;
  }
  if (c == 't' || c == 'f' || c == 'n') {
    token = Token::Literal;
    literal = c == 't' ? "rue" : c == 'f' ? "alse" : "ull";
    return true;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    token = Token::Number;
    number = c == '-' ? Number::Sign : c == '0' ? Number::Zero : Number::Integer;
    return true;
  }
  return false;
}
