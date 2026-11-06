#pragma once
#include <string>
using String = std::string;
namespace base64 {
inline String encode(const char*) { return "encoded"; }
}  // namespace base64
