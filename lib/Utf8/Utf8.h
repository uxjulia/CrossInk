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

// Canonical composition (NFC) for Latin / Vietnamese combining marks and modern
// Hangul jamo. Needed because the device fonts have no combining-mark positioning
// or decomposed Hangul glyphs, so NFD text otherwise renders broken or blank.
std::string utf8ComposeNfc(const std::string& in);

// Compose a null-terminated display buffer without allocating or growing it.
// Uncomposed bytes (including malformed UTF-8) are preserved unchanged.
void utf8ComposeNfcInPlace(char* buffer);

// Returns true when text contains at least one Unicode letter/number-like
// codepoint that can be sent to dictionary lookup. Punctuation, symbols,
// whitespace, and combining marks do not count by themselves.
bool utf8ContainsLookupCharacter(const char* text);
bool utf8ContainsLookupCharacter(const std::string& text);

// Trim punctuation, symbols, and whitespace from the edges of a UTF-8 lookup
// token while preserving internal punctuation (for example l'ete), non-Latin
// scripts, and trailing combining marks. The result is composed to NFC where
// the firmware's compact composition table has a matching entry.
std::string utf8CleanLookupWord(const std::string& text);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for any codepoint in a CJK script block (Han, Kana, Hangul, Bopomofo,
// radicals, and CJK punctuation/compatibility/enclosed forms). Used for fallback font
// selection — deliberately broader than utf8IsCjkBreakable, whose ranges are tuned to
// implicit line-break opportunities and must not grow without rethinking layout.
inline bool utf8IsCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x2E80 && cp <= 0x2FDF)     // CJK Radicals Supplement, Kangxi Radicals
         || (cp >= 0x3000 && cp <= 0x33FF)     // CJK punctuation, Kana, Bopomofo, Hangul Compat
                                               // Jamo, Kanbun, strokes, enclosed + compat forms
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xA960 && cp <= 0xA97F)     // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)     // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE10 && cp <= 0xFE1F)     // Vertical Forms
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2EBEF)   // CJK Extensions B-F
         || (cp >= 0x2F800 && cp <= 0x2FA1F)   // CJK Compatibility Ideographs Supplement
         || (cp >= 0x30000 && cp <= 0x323AF);  // CJK Extensions G-H
}

// Returns true for CJK letters, syllables, and digits that can form a dictionary
// lookup. Unlike utf8IsCjkCodepoint(), this deliberately excludes punctuation
// and compatibility symbols so callers can continue to skip standalone marks.
inline bool utf8IsCjkWordCharacter(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3040 && cp <= 0x30FF)     // Hiragana and Katakana
         || (cp >= 0x3100 && cp <= 0x312F)     // Bopomofo
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x31A0 && cp <= 0x31FF)     // Bopomofo Extended, Katakana Extensions
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xA960 && cp <= 0xA97F)     // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)     // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFF10 && cp <= 0xFF19)     // Fullwidth digits
         || (cp >= 0xFF21 && cp <= 0xFF3A)     // Fullwidth Latin uppercase
         || (cp >= 0xFF41 && cp <= 0xFF5A)     // Fullwidth Latin lowercase
         || (cp >= 0xFF66 && cp <= 0xFFDC)     // Halfwidth Katakana and Hangul
         || (cp >= 0x20000 && cp <= 0x2EBEF)   // CJK Extensions B-F
         || (cp >= 0x2F800 && cp <= 0x2FA1F)   // CJK Compatibility Ideographs Supplement
         || (cp >= 0x30000 && cp <= 0x323AF);  // CJK Extensions G-H
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}

// Variation selectors modify the preceding glyph's presentation. The bitmap
// font pipeline has no variation-selector lookup, so they must not fall back
// to a visible replacement glyph or consume layout width on their own.
inline bool utf8IsVariationSelector(const uint32_t cp) {
  return (cp >= 0x180B && cp <= 0x180D) || cp == 0x180F || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF);
}
