#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace OpdsDownloadFolder {
constexpr size_t MAX_LENGTH = 63;

// Empty inherits the shared default; '/' is an explicit SD-root override.
// Called only while editing or starting a download, never on the render path.
inline bool normalize(std::string_view input, std::string& output) {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);
  if (input.size() > MAX_LENGTH) return false;
  std::string folder;
  if (!input.empty()) {
    folder.reserve(input.size() + 1);
    folder = "/";
    for (const unsigned char c : input) {
      if (c < 32 || c == 127 || std::string_view("\\<>:\"|?*").find(c) != std::string_view::npos) return false;
      if (c != '/' || folder.back() != '/') folder += static_cast<char>(c);
    }
    if (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    if (folder.size() > MAX_LENGTH) return false;
    for (size_t start = 1; start < folder.size();) {
      const size_t end = folder.find('/', start);
      const auto part = std::string_view(folder).substr(start, end == std::string::npos ? end : end - start);
      if (part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  output = std::move(folder);
  return true;
}

inline const char* effective(const std::string& overrideFolder, const char* defaultFolder) {
  return overrideFolder.empty() ? defaultFolder : overrideFolder.c_str();
}
}  // namespace OpdsDownloadFolder
