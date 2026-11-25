#pragma once
#include <string>
namespace obfuscation {
enum class DecodeStatus { INVALID, EMPTY, LEGACY, CURRENT };
inline std::string obfuscateToBase64(const std::string& value) { return value; }
inline std::string deobfuscateFromBase64(const char* value, DecodeStatus* status) {
  *status = *value ? DecodeStatus::CURRENT : DecodeStatus::EMPTY;
  return value;
}
}  // namespace obfuscation
