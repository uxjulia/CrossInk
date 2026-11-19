#pragma once
#include <string_view>

// Pinned SecureNet cannot distinguish failed chunked-trailer reads from the
// final empty trailer line. Keep only fixed-length responses until that SDK
// contract changes; never retain a socket with potentially unread trailer bytes.
inline bool retainHttpSessionFraming(bool hasContentLength, std::string_view transferEncoding) {
  return hasContentLength && transferEncoding.empty();
}
