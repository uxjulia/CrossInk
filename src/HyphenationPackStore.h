#pragma once

#include <Epub/hyphenation/LanguageRegistry.h>
#include <HyphenationPack.h>

namespace HyphenationPackStore {
constexpr char DIRECTORY[] = "/.crossink/hyphenation";
bool begin();
bool lookup(const char* code, ExternalHyphenationPatterns& out);
bool isInstalled(const char* code);
bool hasSource(const char* code);
// Verifies an SD source and available inactive-bank capacity without writing flash.
hyphenation_pack::Result validateSource(const char* path, const char* code, void* context = nullptr,
                                        bool (*continueWork)(void*) = nullptr);
hyphenation_pack::Result install(const char* code);
hyphenation_pack::Result remove(const char* code);
}  // namespace HyphenationPackStore
