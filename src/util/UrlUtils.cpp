#include "UrlUtils.h"

#include <cstdio>

namespace UrlUtils {
namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool shouldEncode(const unsigned char c) {
  if (c <= 0x20 || c >= 0x7f) return true;
  switch (c) {
    case '"':
    case '<':
    case '>':
    case '\\':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
      return true;
    default:
      return false;
  }
}
}  // namespace

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}

std::string extractHost(const std::string& url) {
  const size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    // No protocol, find first slash
    const size_t firstSlash = url.find('/');
    return firstSlash == std::string::npos ? url : url.substr(0, firstSlash);
  }
  // Find the first slash after the protocol
  const size_t hostStart = protocolEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

std::string encodeUnsafeUrlChars(const std::string& url) {
  std::string out;
  out.reserve(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (c == '%' && i + 2 < url.size() && isHexDigit(url[i + 1]) && isHexDigit(url[i + 2])) {
      out += url[i];
      out += url[i + 1];
      out += url[i + 2];
      i += 2;
    } else if (c == '%' || shouldEncode(c)) {
      char encoded[4];
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      out += encoded;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  // If path is already an absolute URL (has protocol), use it directly
  if (path.find("://") != std::string::npos) {
    return encodeUnsafeUrlChars(path);
  }
  const std::string urlWithProtocol = ensureProtocol(serverUrl);
  if (path.empty()) {
    return encodeUnsafeUrlChars(urlWithProtocol);
  }
  if (path[0] == '/') {
    // Absolute path - use just the host
    return encodeUnsafeUrlChars(extractHost(urlWithProtocol) + path);
  }
  // Relative path - strip query string from base before appending
  std::string base = urlWithProtocol;
  const size_t queryPos = base.find('?');
  if (queryPos != std::string::npos) {
    base.resize(queryPos);
  }
  if (base.back() == '/') {
    return encodeUnsafeUrlChars(base + path);
  }
  return encodeUnsafeUrlChars(base + "/" + path);
}

std::string resolveReference(const std::string& baseUrl, const std::string& reference) {
  const size_t scheme = reference.find("://");
  if (scheme != std::string::npos && scheme < reference.find_first_of("/?#")) return encodeUnsafeUrlChars(reference);
  const std::string base = ensureProtocol(baseUrl);
  const std::string cleanBase = base.substr(0, base.find_first_of("?#"));
  const std::string origin = extractHost(cleanBase);
  if (reference.rfind("//", 0) == 0) return encodeUnsafeUrlChars(base.substr(0, base.find(':') + 1) + reference);
  const std::string withoutFragment = base.substr(0, base.find('#'));
  if (reference.empty()) return encodeUnsafeUrlChars(withoutFragment);
  if (reference[0] == '#') return encodeUnsafeUrlChars(withoutFragment + reference);
  if (reference[0] == '?') return encodeUnsafeUrlChars(cleanBase + reference);

  const size_t suffix = reference.find_first_of("?#");
  std::string path = reference.substr(0, suffix);
  if (path[0] != '/') {
    const std::string basePath = cleanBase.substr(origin.size());
    const size_t slash = basePath.rfind('/');
    path = (slash == std::string::npos ? "/" : basePath.substr(0, slash + 1)) + path;
  }

  // RFC 3986 dot-segment removal, retaining meaningful duplicate slashes.
  std::string normalized;
  normalized.reserve(path.size());
  size_t pos = 0;
  const auto removeLast = [&]() {
    const size_t slash = normalized.rfind('/');
    normalized.resize(slash == std::string::npos ? 0 : slash);
  };
  while (pos < path.size()) {
    if (path.compare(pos, 3, "/./") == 0) {
      pos += 2;
    } else if (path.compare(pos, 4, "/../") == 0) {
      pos += 3;
      removeLast();
    } else if (path.compare(pos, std::string::npos, "/.") == 0) {
      normalized += '/';
      break;
    } else if (path.compare(pos, std::string::npos, "/..") == 0) {
      removeLast();
      normalized += '/';
      break;
    } else {
      const size_t next = path.find('/', pos + 1);
      normalized.append(path, pos, next == std::string::npos ? next : next - pos);
      if (next == std::string::npos) break;
      pos = next;
    }
  }
  return encodeUnsafeUrlChars(origin + normalized + (suffix == std::string::npos ? "" : reference.substr(suffix)));
}

}  // namespace UrlUtils

std::string UrlUtils::forLog(const std::string& url) {
  std::string safe = url.substr(0, url.find_first_of("?#"));
  const auto scheme = safe.find("://");
  const auto start = scheme == std::string::npos ? 0 : scheme + 3;
  const auto end = safe.find('/', start);
  const auto at = safe.rfind('@', end == std::string::npos ? safe.size() : end);
  if (at != std::string::npos && at >= start) safe.erase(start, at - start + 1);
  return safe;
}
