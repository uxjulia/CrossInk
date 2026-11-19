#pragma once
#include <WString.h>

#include <string>
#include <string_view>
#include <vector>

#include "NaturalSort.h"

class HalFile;

namespace FsHelpers {

// True when the last openNextFile() returned no entry because the wrapper
// allocation or underlying directory read failed, rather than normal EOF.
bool directoryIterationFailed(const HalFile& directory);

// Validates that a directory can be walked to normal EOF.
bool directoryCanBeEnumerated(const char* path);

enum class DirectoryEntryVisibility { Visible, Missing, IterationFailed };

// Checks whether a direct child is reachable through normal directory
// enumeration, using its stored name in case FAT normalized the requested path.
DirectoryEntryVisibility directoryEntryVisibility(const char* directoryPath, const char* entryPath);

// Resolves a direct child of the SD-card root regardless of ASCII case and
// writes its on-disk path to resolvedPath.
bool resolveRootDirectoryIgnoreCase(const char* expectedPath, char* resolvedPath, size_t resolvedPathSize,
                                    bool* iterationFailed = nullptr);

std::string decodeUriEscapes(const std::string& path);

std::string normalisePath(const std::string& path);

// Numeric-aware, case-insensitive comparison ("2" < "10"). Returns true when str1 orders
// before str2. Same ordering sortFileList applies within the file/directory groups.
bool naturalLess(const std::string& str1, const std::string& str2);

void sortFileList(std::vector<std::string>& strs);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);
inline bool hasCssExtension(const String& fileName) {
  return hasCssExtension(std::string_view{fileName.c_str(), fileName.length()});
}
std::string extractFolderPath(const std::string& filePath);

// Rejects a path component that could escape the directory it is joined to.
// Repeated dots inside a normal filename remain valid.
bool isSafePathComponent(std::string_view name);
inline bool isSafePathComponent(const String& name) {
  return isSafePathComponent(std::string_view{name.c_str(), name.length()});
}

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

}  // namespace FsHelpers
