#pragma once

#include <string_view>

namespace library {
// Normalize only leading root aliases, not nested user folder names.
inline std::string_view normalizeLibraryRootAliases(std::string_view path) {
  while (!path.empty()) {
    const auto end = path.find('/');
    const auto part = path.substr(0, end);
    if (!part.empty() && part != ".") break;
    if (end == std::string_view::npos) return {};
    path.remove_prefix(end + 1);
  }
  return path;
}

// Root-only resource folders stay accessible in File Browser. No allocations.
inline bool isLibraryResourceDirectory(std::string_view path) {
  path = normalizeLibraryRootAliases(path);
  const auto root = path.substr(0, path.find('/'));
  for (const auto name : {std::string_view("fonts"), std::string_view("dictionaries"), std::string_view("plugins"),
                          std::string_view("sleep"), std::string_view("sleep-overlay")}) {
    if (root.size() != name.size()) continue;
    bool match = true;
    for (size_t i = 0; i < root.size(); ++i) {
      const char c = root[i] >= 'A' && root[i] <= 'Z' ? root[i] + ('a' - 'A') : root[i];
      if (c != name[i]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}
}  // namespace library
