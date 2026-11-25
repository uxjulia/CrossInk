#pragma once

#include <cstddef>
#include <string>

#include "LanguageHyphenator.h"

struct LanguageEntry {
  const char* cliName;
  const char* primaryTag;
  const LanguageHyphenator* hyphenator;
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
uint32_t getLanguagePatternIdentity(const char* primaryTag);

// Returns the Liang-backed hyphenator for a given primary language tag (e.g., "en", "fr").
const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag);

// Exposes the list of supported languages primarily for tooling/tests.
LanguageEntryView getLanguageEntries();
