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
                                       {"dutch", "nl", nullptr}}};
  return kEntries;
}

ExternalHyphenationLookup externalLookup = nullptr;
SerializedHyphenationPatterns externalPatterns{0, nullptr, 0};
LanguageHyphenator externalHyphenator(externalPatterns, isLatinLetter, toLowerLatin);

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
  if (!entry) return nullptr;
  if (entry->hyphenator) return entry->hyphenator;
  ExternalHyphenationPatterns found{};
  if (!externalLookup || !externalLookup(entry->primaryTag, found)) return nullptr;
  externalPatterns = found.patterns;
  const bool cyrillic = primaryTag == "ru" || primaryTag == "uk";
  externalHyphenator.configure(cyrillic ? isCyrillicLetter : isLatinLetter, cyrillic ? toLowerCyrillic : toLowerLatin);
  return &externalHyphenator;
}

uint32_t getLanguagePatternIdentity(const char* primaryTag) {
  const auto* entry = findLanguageEntry(primaryTag);
  if (!entry) return 0;
  // Built-in pattern changes already require the section format version bump.
  if (entry->hyphenator) return 1;
  ExternalHyphenationPatterns found{};
  return externalLookup && externalLookup(entry->primaryTag, found) ? found.identity : 0;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
