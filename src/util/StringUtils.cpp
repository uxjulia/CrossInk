#include "StringUtils.h"

#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

constexpr size_t MAX_EXTENSION_BYTES = 16;

bool isInvalidFilenameCodepoint(uint32_t cp) {
  return cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
         cp == '|';
}

std::string trimFilenameEdges(const std::string& name) {
  size_t start = 0;
  while (start < name.size() && (name[start] == ' ' || name[start] == '.')) {
    start++;
  }

  size_t end = name.size();
  while (end > start && (name[end - 1] == ' ' || name[end - 1] == '.')) {
    end--;
  }

  return name.substr(start, end - start);
}

size_t findSafeExtensionStart(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) {
    return std::string::npos;
  }

  const size_t extensionBytes = name.size() - dot;
  if (extensionBytes > MAX_EXTENSION_BYTES) {
    return std::string::npos;
  }

  for (size_t i = dot + 1; i < name.size(); i++) {
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) {
      return std::string::npos;
    }
  }

  return dot;
}

std::string sanitizeFilenamePart(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (isInvalidFilenameCodepoint(cp)) {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }

  return result.empty() ? "book" : result;
}

}  // namespace

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  const std::string trimmedName = trimFilenameEdges(name);
  const size_t extensionStart = findSafeExtensionStart(trimmedName);
  if (extensionStart != std::string::npos) {
    const std::string extension = trimmedName.substr(extensionStart);
    if (extension.size() < maxBytes) {
      const size_t baseBudget = maxBytes - extension.size();
      std::string base = sanitizeFilenamePart(trimmedName.substr(0, extensionStart), baseBudget);
      if (base.size() > baseBudget) {
        base.resize(baseBudget);
      }
      if (!base.empty()) {
        return base + extension;
      }
    }
  }

  return sanitizeFilenamePart(name, maxBytes);
}

void formatByteSize(const size_t bytes, char* const buffer, const size_t bufferSize) {
  if (bufferSize == 0) return;
  if (bytes >= 1024 * 1024) {
    snprintf(buffer, bufferSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buffer, bufferSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buffer, bufferSize, "%zu B", bytes);
  }
}

}  // namespace StringUtils
