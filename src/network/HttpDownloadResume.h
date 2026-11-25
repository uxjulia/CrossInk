#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

// Range validation and bounded recovery, adapted from FreeInk ResumableFetch.
// Shared by both CrossInk transports; no HTTP, TLS or filesystem ownership here.
namespace HttpDownloadResume {
inline bool range(std::string_view value, size_t offset, size_t knownTotal, bool hasLength, size_t length,
                  size_t& total, size_t& endOffset) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  if (value.substr(0, 6) != "bytes ") return false;
  const char* cursor = value.data() + 6;
  const char* end = value.data() + value.size();
  const auto number = [&](size_t& out, char separator) {
    const auto parsed = std::from_chars(cursor, end, out);
    if (parsed.ec != std::errc{}) return false;
    cursor = parsed.ptr;
    if (!separator) return cursor == end;
    if (cursor == end || *cursor != separator) return false;
    ++cursor;
    return true;
  };
  size_t first = 0, last = 0, size = 0;
  if (!number(first, '-') || !number(last, '/') || !number(size, 0) || first != offset || first > last ||
      last >= size || (knownTotal && size != knownTotal) || (hasLength && length != last - first + 1))
    return false;
  total = size;
  endOffset = last + 1;
  return true;
}

inline bool accepts(size_t downloaded, size_t endOffset, size_t count) {
  return count <= std::numeric_limits<size_t>::max() - downloaded &&
         (!endOffset || (downloaded <= endOffset && count <= endOffset - downloaded));
}

struct RetryBudget {
  uint8_t attempts = 0;
  uint8_t stalled = 0;
  bool again(size_t before, size_t after) {
    ++attempts;
    stalled = after > before ? 0 : stalled + 1;
    return attempts < 20 && stalled < 3;
  }
};
}  // namespace HttpDownloadResume
