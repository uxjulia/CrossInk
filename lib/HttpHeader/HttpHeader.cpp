#include "HttpHeader.h"

#include <cstring>
namespace {
std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}
bool equalsIgnoreCase(std::string_view value, const char* expected) {
  if (value.size() != strlen(expected)) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != expected[i]) return false;
  }
  return true;
}
}  // namespace
bool isValidHttpHeader(std::string_view name, std::string_view value) {
  if (name.size() + value.size() > 160) return false;
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          (c && strchr("!#$%&'*+-.^_`|~", c))))
      return false;
  }
  for (unsigned char c : value)
    if (c < 32 || c == 127) return false;
  for (const char* reserved : {"host", "content-length", "transfer-encoding", "connection", "x-auth-user", "x-auth-key",
                               "accept", "content-type", "authorization"})
    if (equalsIgnoreCase(name, reserved)) return false;
  return true;
}
bool parseHttpHeaderLine(std::string_view line, HttpHeader& header) {
  line = trim(line);
  const size_t colon = line.find(':');
  const auto name = trim(line.substr(0, colon));
  const auto value = colon == std::string_view::npos ? std::string_view{} : trim(line.substr(colon + 1));
  if (!isValidHttpHeader(name, value)) return false;
  header = {std::string(name), std::string(value)};
  return true;
}
std::string formatHttpHeaderLine(const HttpHeader& header, bool maskValue) {
  if (header.name.empty()) return {};
  if (header.value.empty()) return header.name;
  return header.name + ": " + (maskValue ? "******" : header.value);
}
