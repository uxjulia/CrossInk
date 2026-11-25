#pragma once
#include <string>
struct BookSyncFile {
  static std::string documentId(const std::string&) { return "0123456789abcdef0123456789abcdef"; }
};
