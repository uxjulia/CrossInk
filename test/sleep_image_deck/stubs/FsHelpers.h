#pragma once
#include <HalStorage.h>

#include <cctype>
#include <string_view>
namespace FsHelpers {
inline bool checkFileExtension(std::string_view name, const char* extension) {
  const std::string_view ext(extension);
  if (name.size() < ext.size()) return false;
  name.remove_prefix(name.size() - ext.size());
  for (size_t i = 0; i < ext.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(name[i])) != ext[i]) return false;
  return true;
}
inline bool hasBmpExtension(std::string_view name) { return checkFileExtension(name, ".bmp"); }
inline bool hasPngExtension(std::string_view name) { return checkFileExtension(name, ".png"); }
inline bool directoryIterationFailed(const HalFile& file) { return file.iterationFailed(); }
inline bool resolveRootDirectoryIgnoreCase(const char* expected, char* output, size_t length) {
  auto dir = Storage.open("/");
  for (auto child = dir.openNextFile(); child; child = dir.openNextFile()) {
    char name[256];
    child.getName(name, sizeof(name));
    const bool match =
        child.isDirectory() && checkFileExtension(name, expected + 1) && std::strlen(name) == std::strlen(expected + 1);
    child.close();
    if (match) {
      std::snprintf(output, length, "/%s", name);
      dir.close();
      return true;
    }
  }
  dir.close();
  return false;
}
}  // namespace FsHelpers
