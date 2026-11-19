#include "FsHelpers.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace FsHelpers {

bool directoryIterationFailed(const HalFile& directory) {
  return directory.allocationFailed() || directory.iterationFailed();
}

bool directoryCanBeEnumerated(const char* path) {
  HalFile directory = Storage.open(path);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return false;
  }

  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    entry.close();
    yield();
  }

  const bool complete = !directoryIterationFailed(directory);
  if (!complete) {
    LOG_ERR("FS", "Directory listing failed before EOF: %s", path);
  }
  directory.close();
  return complete;
}

DirectoryEntryVisibility directoryEntryVisibility(const char* directoryPath, const char* entryPath) {
  if (!directoryPath || !entryPath || entryPath[0] == '\0') return DirectoryEntryVisibility::Missing;

  HalFile expectedEntry = Storage.open(entryPath);
  if (!expectedEntry) {
    return expectedEntry.allocationFailed() ? DirectoryEntryVisibility::IterationFailed
                                            : DirectoryEntryVisibility::Missing;
  }

  char expectedName[256] = {};  // FAT long filenames are at most 255 bytes.
  expectedEntry.getName(expectedName, sizeof(expectedName));
  expectedEntry.close();
  if (expectedName[0] == '\0') return DirectoryEntryVisibility::IterationFailed;

  HalFile directory = Storage.open(directoryPath);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return DirectoryEntryVisibility::IterationFailed;
  }

  char name[256];  // FAT long filenames are at most 255 bytes.
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    entry.getName(name, sizeof(name));
    entry.close();
    if (strcmp(name, expectedName) == 0) {
      directory.close();
      return DirectoryEntryVisibility::Visible;
    }
    yield();
  }

  if (directoryIterationFailed(directory)) {
    LOG_ERR("FS", "Directory listing failed before finding %s in %s", expectedName, directoryPath);
    directory.close();
    return DirectoryEntryVisibility::IterationFailed;
  }
  directory.close();
  return DirectoryEntryVisibility::Missing;
}

bool resolveRootDirectoryIgnoreCase(const char* expectedPath, char* resolvedPath, const size_t resolvedPathSize,
                                    bool* iterationFailed) {
  if (iterationFailed) *iterationFailed = false;
  if (!expectedPath || expectedPath[0] != '/' || expectedPath[1] == '\0' || strchr(expectedPath + 1, '/') ||
      !resolvedPath || resolvedPathSize == 0) {
    return false;
  }

  HalFile exact = Storage.open(expectedPath);
  if (exact && exact.isDirectory()) {
    const int written = snprintf(resolvedPath, resolvedPathSize, "%s", expectedPath);
    exact.close();
    return written > 0 && static_cast<size_t>(written) < resolvedPathSize;
  }
  const bool allocationFailed = exact.allocationFailed();
  exact.close();
  if (allocationFailed && iterationFailed) {
    *iterationFailed = true;
    return false;
  }

  HalFile root = Storage.open("/");
  if (!root || !root.isDirectory()) {
    if (iterationFailed) *iterationFailed = true;
    root.close();
    return false;
  }

  char name[256];  // FAT long filenames are at most 255 bytes.
  const char* expectedName = expectedPath + 1;
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || strcasecmp(name, expectedName) != 0) continue;

    const int written = snprintf(resolvedPath, resolvedPathSize, "/%s", name);
    root.close();
    return written > 0 && static_cast<size_t>(written) < resolvedPathSize;
  }

  if (iterationFailed) *iterationFailed = directoryIterationFailed(root);
  root.close();
  return false;
}

namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

uint8_t hexValue(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return static_cast<uint8_t>(10 + (c - 'A'));
}
}  // namespace

std::string decodeUriEscapes(const std::string& path) {
  std::string decoded;
  decoded.reserve(path.size());

  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '%' && i + 2 < path.size() && isHexDigit(path[i + 1]) && isHexDigit(path[i + 2])) {
      const uint8_t value = static_cast<uint8_t>((hexValue(path[i + 1]) << 4) | hexValue(path[i + 2]));
      decoded += static_cast<char>(value);
      i += 2;
      continue;
    }

    decoded += path[i];
  }

  return decoded;
}

std::string normalisePath(const std::string& path) {
  std::vector<std::string_view> components;
  components.reserve(8);  // Eight nested folders is more than we might expect

  size_t start = 0;
  for (size_t i = 0; i <= path.length(); ++i) {
    if (i == path.length() || path[i] == '/') {
      if (i > start) {
        std::string_view component(path.data() + start, i - start);
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
      }
      start = i + 1;
    }
  }

  if (components.empty()) {
    return "";
  }

  size_t total_len = 0;
  for (const auto& c : components) {
    total_len += c.length() + 1;
  }

  std::string result;
  result.reserve(total_len - 1);

  for (size_t i = 0; i < components.size(); ++i) {
    if (i > 0) {
      result += '/';
    }
    result.append(components[i].data(), components[i].length());
  }

  return result;
}

bool naturalLess(const std::string& str1, const std::string& str2) {
  return naturalCompare(str1.c_str(), str2.c_str()) < 0;
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    const bool isDir1 = str1.back() == '/';
    const bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    return naturalLess(str1, str2);
  });
}

bool checkFileExtension(std::string_view fileName, const char* extension) {
  const size_t extLen = strlen(extension);
  if (fileName.length() < extLen) {
    return false;
  }

  const size_t offset = fileName.length() - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (tolower(static_cast<unsigned char>(fileName[offset + i])) !=
        tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

bool hasJpgExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".jpg") || checkFileExtension(fileName, ".jpeg");
}

bool hasPngExtension(std::string_view fileName) { return checkFileExtension(fileName, ".png"); }

bool hasBmpExtension(std::string_view fileName) { return checkFileExtension(fileName, ".bmp"); }

bool hasGifExtension(std::string_view fileName) { return checkFileExtension(fileName, ".gif"); }

bool hasEpubExtension(std::string_view fileName) { return checkFileExtension(fileName, ".epub"); }

bool hasXtcExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".xtc") || checkFileExtension(fileName, ".xtch");
}

bool hasTxtExtension(std::string_view fileName) { return checkFileExtension(fileName, ".txt"); }

bool hasMarkdownExtension(std::string_view fileName) { return checkFileExtension(fileName, ".md"); }

bool hasCssExtension(std::string_view fileName) { return checkFileExtension(fileName, ".css"); }

std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool isSafePathComponent(const std::string_view name) {
  return !name.empty() && name.find_first_of("/\\") == std::string_view::npos && name != "." && name != "..";
}

void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (maxLen == 0) {
    return;
  }

  size_t i = 0;
  for (; i < maxLen - 1 && input[i] != '\0'; i++) {
    const char c = input[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        c == ' ' || (c > 0x00 && c <= 0x1f)) {
      output[i] = '-';
    } else {
      output[i] = c;
    }
  }
  // If the last character was cut in half, drop its leftover bytes.
  i = static_cast<size_t>(utf8SafeTruncateBuffer(output, static_cast<int>(i)));
  output[i] = '\0';
}

}  // namespace FsHelpers
