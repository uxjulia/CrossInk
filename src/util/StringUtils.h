#pragma once

#include <cstddef>
#include <string>

namespace StringUtils {

constexpr size_t kDefaultMaxFilenameBytes = 150;

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = kDefaultMaxFilenameBytes);

/** Format a byte count as "123 B", "45 KB" or "1.2 MB". */
void formatByteSize(size_t bytes, char* buffer, size_t bufferSize);

}  // namespace StringUtils
