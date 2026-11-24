#include "LanguageRegistry.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

using EntryArray = std::array<LanguageEntry, HYPHENATION_LANGUAGE_COUNT>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", nullptr},
                                       {"german", "de", nullptr},
                                       {"russian", "ru", nullptr},
                                       {"spanish", "es", nullptr},
                                       {"italian", "it", nullptr},
                                       {"polish", "pl", nullptr},
                                       {"portuguese", "pt", nullptr},
                                       {"swedish", "sv", nullptr},
                                       {"ukrainian", "uk", nullptr},
                                       {"dutch", "nl", nullptr},
                                       {"albanian", "sq", nullptr, "Shqip"},
                                       {"belarusian", "be", nullptr, "Беларуская"},
                                       {"bulgarian", "bg", nullptr, "Български"},
                                       {"catalan", "ca", nullptr, "Català"},
                                       {"croatian", "hr", nullptr, "Hrvatski"},
                                       {"czech", "cs", nullptr, "Čeština"},
                                       {"danish", "da", nullptr, "Dansk"},
                                       {"finnish", "fi", nullptr, "Suomi"},
                                       {"galician", "gl", nullptr, "Galego"},
                                       {"icelandic", "is", nullptr, "Íslenska"},
                                       {"kurmanji", "ku", nullptr, "Kurmancî"},
                                       {"latin", "la", nullptr, "Latina"},
                                       {"lithuanian", "lt", nullptr, "Lietuvių"},
                                       {"mongolian", "mn", nullptr, "Монгол"},
                                       {"norwegian", "no", nullptr, "Norsk"},
                                       {"serbian", "sr", nullptr, "Српски"},
                                       {"slovenian", "sl", nullptr, "Slovenščina"},
                                       {"turkish", "tr", nullptr, "Türkçe"}}};
  return kEntries;
}

ExternalHyphenationLookup externalLookup = nullptr;
SerializedHyphenationPatterns externalPatterns{0, nullptr, 0};
LanguageHyphenator externalHyphenator(externalPatterns, isLatinLetter, toLowerLatin);

bool isPrimaryTag(const char* code) {
  return code && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z' && !code[2];
}

uint32_t toLowerSupplementalCyrillic(uint32_t cp) {
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
  if (((cp >= 0x0460 && cp <= 0x0480) || (cp >= 0x048A && cp <= 0x04BE) || (cp >= 0x04D0 && cp <= 0x052E)) && !(cp & 1))
    return cp + 1;
  if (cp >= 0x04C1 && cp <= 0x04CD && (cp & 1)) return cp + 1;
  if (cp == 0x04C0) return 0x04CF;
  return toLowerCyrillic(cp);
}

uint32_t toLowerTurkish(uint32_t cp) {
  if (cp == 'I') return 0x0131;
  if (cp == 0x0130) return 'i';
  return toLowerLatin(cp);
}

uint32_t toLowerExternal(uint32_t cp) {
  return isCyrillicLetter(cp) ? toLowerSupplementalCyrillic(cp) : toLowerLatin(cp);
}

}  // namespace

// Adapted from CrossPoint Reader PR #3706 (MIT): external pattern provider.
void setExternalHyphenationLookup(ExternalHyphenationLookup lookup) { externalLookup = lookup; }

const LanguageEntry* findLanguageEntry(const char* primaryTag) {
  if (!primaryTag) return nullptr;
  for (const auto& entry : entries())
    if (!std::strcmp(primaryTag, entry.primaryTag)) return &entry;
  return nullptr;
}

bool isSupplementalHyphenationCode(const char* primaryTag) {
  if (!primaryTag) return false;
  const auto& all = entries();
  return std::any_of(all.begin() + BASE_HYPHENATION_LANGUAGE_COUNT, all.end(),
                     [primaryTag](const auto& entry) { return std::strcmp(entry.primaryTag, primaryTag) == 0; });
}

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  if (primaryTag.size() != 2 || !isPrimaryTag(primaryTag.c_str())) return nullptr;
  const auto* entry = findLanguageEntry(primaryTag.c_str());
  if (entry && entry->hyphenator) return entry->hyphenator;
  ExternalHyphenationPatterns found{};
  if (!externalLookup || !externalLookup(primaryTag.c_str(), found)) return nullptr;
  externalPatterns = found.patterns;
  if (primaryTag == "ru" || primaryTag == "uk")
    externalHyphenator.configure(isCyrillicLetter, toLowerCyrillic);
  else if (primaryTag == "be" || primaryTag == "bg" || primaryTag == "mn" || primaryTag == "sr")
    externalHyphenator.configure(isCyrillicLetter, toLowerSupplementalCyrillic);
  else if (primaryTag == "tr")
    externalHyphenator.configure(isLatinLetter, toLowerTurkish);
  else if (!entry)
    externalHyphenator.configure(isAlphabetic, toLowerExternal);
  else
    externalHyphenator.configure(isLatinLetter, toLowerLatin);
  return &externalHyphenator;
}

uint32_t getLanguagePatternIdentity(const char* primaryTag) {
  if (!isPrimaryTag(primaryTag)) return 0;
  const auto* entry = findLanguageEntry(primaryTag);
  // Built-in pattern changes already require the section format version bump.
  if (entry && entry->hyphenator) return 1;
  ExternalHyphenationPatterns found{};
  return externalLookup && externalLookup(primaryTag, found) ? found.identity : 0;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
