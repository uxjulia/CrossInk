#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F)        // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Punctuation that should not be left hanging at the end of a line.
inline bool utf8IsCjkOpeningPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x3008:  // left angle bracket
    case 0x300A:  // left double angle bracket
    case 0x300C:  // left corner bracket
    case 0x300E:  // left white corner bracket
    case 0x3010:  // left black lenticular bracket
    case 0x3014:  // left tortoise shell bracket
    case 0x3016:  // left white lenticular bracket
    case 0x3018:  // left white tortoise shell bracket
    case 0x301A:  // left white square bracket
    case 0xFF08:  // fullwidth left parenthesis
    case 0xFF3B:  // fullwidth left square bracket
    case 0xFF5B:  // fullwidth left curly bracket
      return true;
    default:
      return false;
  }
}

// Punctuation that should not start a line.
inline bool utf8IsCjkClosingPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x3001:  // ideographic comma
    case 0x3002:  // ideographic full stop
    case 0x3009:  // right angle bracket
    case 0x300B:  // right double angle bracket
    case 0x300D:  // right corner bracket
    case 0x300F:  // right white corner bracket
    case 0x3011:  // right black lenticular bracket
    case 0x3015:  // right tortoise shell bracket
    case 0x3017:  // right white lenticular bracket
    case 0x3019:  // right white tortoise shell bracket
    case 0x301B:  // right white square bracket
    case 0xFF01:  // fullwidth exclamation mark
    case 0xFF09:  // fullwidth right parenthesis
    case 0xFF0C:  // fullwidth comma
    case 0xFF0E:  // fullwidth full stop
    case 0xFF1A:  // fullwidth colon
    case 0xFF1B:  // fullwidth semicolon
    case 0xFF1F:  // fullwidth question mark
    case 0xFF3D:  // fullwidth right square bracket
    case 0xFF5D:  // fullwidth right curly bracket
      return true;
    default:
      return false;
  }
}

inline bool utf8LanguageTagIsCjk(const std::string& languageTag) {
  if (languageTag.size() < 2) return false;
  const char first = languageTag[0] >= 'A' && languageTag[0] <= 'Z' ? languageTag[0] - 'A' + 'a' : languageTag[0];
  const char second = languageTag[1] >= 'A' && languageTag[1] <= 'Z' ? languageTag[1] - 'A' + 'a' : languageTag[1];
  if ((first == 'j' && second == 'a') || (first == 'z' && second == 'h') || (first == 'k' && second == 'o')) {
    return true;
  }
  if (languageTag.size() < 3) return false;
  const char third = languageTag[2] >= 'A' && languageTag[2] <= 'Z' ? languageTag[2] - 'A' + 'a' : languageTag[2];
  return (first == 'j' && second == 'p' && third == 'n') || (first == 'z' && second == 'h' && third == 'o') ||
         (first == 'c' && second == 'h' && third == 'i') || (first == 'k' && second == 'o' && third == 'r');
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}
