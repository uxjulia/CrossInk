#include "RuledLineLayout.h"

#include <Utf8.h>

#include "hyphenation/HyphenationCommon.h"

void RuledLineLayout::Ink::observeText(const char* text) {
  if (!text || letters == LetterCase::Prose) return;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    if (toLowerLatin(cp) != cp || toLowerCyrillic(cp) != cp) {
      letters = LetterCase::Capitals;
    } else if ((cp < 128 && !(cp >= 'a' && cp <= 'z')) || utf8IsCombiningMark(cp) || (cp >= 0x2000 && cp <= 0x206F) ||
               cp == 0x00A0 || cp == 0x00AB || cp == 0x00BB || cp == 0x00B7) {
      // Digits, punctuation, spaces, accents and guide dots do not decide the baseline.
      continue;
    } else {
      // Lowercase and unfamiliar scripts keep their ordinary text positioning.
      letters = LetterCase::Prose;
      return;
    }
  }
}
