#pragma once
#include "I18n.h"
namespace hyphenation_pack {
enum class Result { Ok, Invalid, Cancelled };
}
namespace HyphenationPackStore {
inline constexpr char DIRECTORY[] = "/.crossink/hyphenation";
inline hyphenation_pack::Result validateSource(const char*, const char*, void* context, bool (*proceed)(void*)) {
  return !proceed(context) ? hyphenation_pack::Result::Cancelled
         : sourceValid     ? hyphenation_pack::Result::Ok
                           : hyphenation_pack::Result::Invalid;
}
}  // namespace HyphenationPackStore
