#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

inline constexpr size_t BASE_HYPHENATION_LANGUAGE_COUNT = 11;
inline constexpr size_t SUPPLEMENTAL_HYPHENATION_LANGUAGE_COUNT = 18;
inline constexpr size_t HYPHENATION_LANGUAGE_COUNT =
    BASE_HYPHENATION_LANGUAGE_COUNT + SUPPLEMENTAL_HYPHENATION_LANGUAGE_COUNT;

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const LanguageHyphenator* hyphenator;
  const char* displayName = nullptr;
};

struct LanguageEntryView {
  const LanguageEntry* data;
  size_t size;

  const LanguageEntry* begin() const { return data; }
  const LanguageEntry* end() const { return data + size; }
};

struct ExternalHyphenationPatterns {
  SerializedHyphenationPatterns patterns;
  uint32_t identity = 0;
};
using ExternalHyphenationLookup = bool (*)(const char*, ExternalHyphenationPatterns&);
void setExternalHyphenationLookup(ExternalHyphenationLookup lookup);
const LanguageEntry* findLanguageEntry(const char* primaryTag);
bool isSupplementalHyphenationCode(const char* primaryTag);
uint32_t getLanguagePatternIdentity(const char* primaryTag);

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr").
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);

// Exposes the list of supported languages primarily for tooling/tests.
LanguageEntryView getLanguageEntries();
