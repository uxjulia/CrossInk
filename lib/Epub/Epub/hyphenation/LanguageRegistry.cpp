#include "LanguageRegistry.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

using EntryArray = std::array<LanguageEntry, 11>;

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
                                       {"finnish", "fi", nullptr}}};
  return kEntries;
}

ExternalHyphenationLookup externalLookup = nullptr;
SerializedHyphenationPatterns externalPatterns{0, nullptr, 0};
LanguageHyphenator externalHyphenator(externalPatterns, isLatinLetter, toLowerLatin);

uint32_t toLowerExternal(uint32_t cp) { return isCyrillicLetter(cp) ? toLowerCyrillic(cp) : toLowerLatin(cp); }
bool isPrimaryTag(const char* code) {
  return code && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z' && !code[2];
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

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto* entry = findLanguageEntry(primaryTag.c_str());
  if (!isPrimaryTag(primaryTag.c_str()) || primaryTag.size() != 2) return nullptr;
  if (entry && entry->hyphenator) return entry->hyphenator;
  ExternalHyphenationPatterns found{};
  if (!externalLookup || !externalLookup(primaryTag.c_str(), found)) return nullptr;
  externalPatterns = found.patterns;
  const bool cyrillic = primaryTag == "ru" || primaryTag == "uk";
  // Legacy packs retain the same word filtering; new local codes need no
  // per-language script registration to use the engine's supported alphabets.
  externalHyphenator.configure(!entry     ? isAlphabetic
                               : cyrillic ? isCyrillicLetter
                                          : isLatinLetter,
                               !entry     ? toLowerExternal
                               : cyrillic ? toLowerCyrillic
                                          : toLowerLatin);
  return &externalHyphenator;
}

uint32_t getLanguagePatternIdentity(const char* primaryTag) {
  const auto* entry = findLanguageEntry(primaryTag);
  if (!isPrimaryTag(primaryTag)) return 0;
  // Built-in pattern changes already require the section format version bump.
  if (entry && entry->hyphenator) return 1;
  ExternalHyphenationPatterns found{};
  return externalLookup && externalLookup(primaryTag, found) ? found.identity : 0;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
