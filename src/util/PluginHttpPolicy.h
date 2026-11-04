#pragma once
#include <cctype>
#include <string>

#include "network/HttpRedirectPolicy.h"
namespace pluginhttp {
inline bool sameOrigin(const std::string& trusted, const std::string& target) {
  HttpRedirectPolicy::Url a, b;
  return HttpRedirectPolicy::parseUrl(trusted, a) && HttpRedirectPolicy::parseUrl(target, b) &&
         HttpRedirectPolicy::sameOrigin(a, b);
}
// A feed field can supply the entire origin, including just the hostname or port.
inline bool trustedDownloadTarget(const std::string& configured, const std::string& browse, const std::string& target) {
  HttpRedirectPolicy::Url origin;
  const bool fixed =
      HttpRedirectPolicy::parseUrl(configured, origin) && origin.host.find_first_of("@{}") == std::string::npos;
  return sameOrigin(fixed ? configured : browse, target);
}
inline bool usesSecretTemplate(const std::string& value) {
  return value.find("{token}") != std::string::npos || value.find("{cfg.") != std::string::npos;
}
inline bool validRequest(const std::string& url, const std::string& method) {
  if (url.size() > 2048 || method.empty() || method.size() > 16) return false;
  for (const unsigned char c : url)
    if (c <= 32 || c == 127 || c == '\\') return false;
  for (const unsigned char c : method)
    if (c < 'A' || c > 'Z') return false;
  HttpRedirectPolicy::Url parsed;
  return HttpRedirectPolicy::parseUrl(url, parsed) && parsed.host.find_first_of("@{}") == std::string::npos;
}
inline bool validHeader(const std::string& name, const std::string& value) {
  if (name.empty() || name.size() > 64 || value.size() > 2048) return false;
  for (const unsigned char c : name)
    if (!std::isalnum(c) && std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos) return false;
  for (const unsigned char c : value)
    if (c < 32 || c == 127) return false;
  for (const char* reserved : {"host", "content-length", "transfer-encoding", "connection", "range"})
    if (HttpRedirectPolicy::equalsIgnoreCase(name, reserved)) return false;
  return true;
}
}  // namespace pluginhttp
