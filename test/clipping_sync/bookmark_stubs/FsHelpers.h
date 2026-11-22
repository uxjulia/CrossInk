#pragma once
#include <string_view>
namespace FsHelpers {
inline bool hasEpubExtension(std::string_view value) { return value.ends_with(".epub"); }
}  // namespace FsHelpers
