#pragma once
// Exercise production discovery and text-arena accounting. Only full source
// contract validation is substituted here; that parser has its own integration tests.
#include <functional>

#include "../../../lib/I18n/I18n.h"
inline bool sourceValid = true;
inline language_cache::Result discoveryFailure = language_cache::Result::Ok;
inline size_t discoveries = 0;
inline std::function<void()> beforeDiscovery;
struct I18nFake {
  language_cache::Result validateSource(const char*, const char*, const char*, const char*, void* context,
                                        bool (*proceed)(void*)) {
    return !proceed(context) ? language_cache::Result::Cancelled
           : sourceValid     ? language_cache::Result::Ok
                             : language_cache::Result::Invalid;
  }
  language_cache::Result discover(I18n::Catalog& catalog, void* context = nullptr, bool (*proceed)(void*) = nullptr,
                                  const I18n::SourceProjection* projection = nullptr) {
    ++discoveries;
    if (beforeDiscovery) beforeDiscovery();
    if (discoveryFailure != language_cache::Result::Ok) return discoveryFailure;
    return I18n::getInstance().discover(catalog, context, proceed, projection);
  }
};
inline I18nFake assetI18n;
#undef I18N
#define I18N assetI18n
