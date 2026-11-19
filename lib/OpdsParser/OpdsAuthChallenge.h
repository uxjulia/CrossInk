// Basic-only streaming adaptation of the pinned FreeInk OpdsAuthDoc.
// See LICENSE.FreeInk. No token grants, refresh tokens, or cross-host login.
#pragma once
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

class OpdsAuthChallenge {
 public:
  OpdsAuthChallenge();
  bool write(const uint8_t* bytes, size_t length);
  bool offersBasic() const { return rootClosed && !invalid && !parser.hasError() && basic; }

 private:
  static void key(void*, const char*, size_t);
  static void value(void*, const char*, size_t);
  static void objectStart(void*);
  static void objectEnd(void*);
  static void arrayStart(void*);
  static void arrayEnd(void*);
  // The SDK SAX tokenizer is permissive about JSON separators. This bounded
  // grammar guard validates syntax before any authentication offer is accepted.
  bool syntax(char);
  bool valueDone();
  enum class Grammar : uint8_t {
    ObjectFirst,
    ObjectKey,
    Colon,
    ObjectValue,
    ObjectComma,
    ArrayFirst,
    ArrayValue,
    ArrayComma
  };
  Grammar grammar[StreamingJsonParser::MAX_NESTING]{};
  uint8_t grammarDepth = 0;
  enum class Token : uint8_t { None, String, Literal, Number } token = Token::None;
  enum class Number : uint8_t {
    Sign,
    Zero,
    Integer,
    Dot,
    Fraction,
    Exponent,
    ExponentSign,
    ExponentDigits
  } number = Number::Integer;
  const char* literal = nullptr;
  bool stringKey = false, escaped = false, syntaxDone = false;
  uint8_t hexDigits = 0;
  StreamingJsonParser parser;
  size_t received = 0;
  uint8_t depth = 0;
  uint8_t authArray = 0;
  uint8_t authObject = 0;
  enum class Key { None, Authentication, Type } pending = Key::None;
  bool basic = false;
  bool rootSeen = false;
  bool rootClosed = false;
  bool invalid = false;
};
