#pragma once
#include <string>
struct KOReaderDocumentId {
  static std::string calculate(const std::string&) { return std::string(32, 'a'); }
};
