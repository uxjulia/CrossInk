#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// Parse once per response, never per body chunk. Names are preserved byte-for-byte;
// rejected names use the caller's generated fallback rather than a modified identity.
namespace HttpDownloadFilename {
constexpr size_t MAX_HEADER_BYTES = 1024;
// Leave room for .part/.old sidecars within the firmware's 255-byte name buffers.
constexpr size_t MAX_NAME_BYTES = 240;

inline bool equalIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}
inline std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}
inline int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool safe(std::string_view name) {
  if (name.empty() || name.size() > MAX_NAME_BYTES || name.front() == '.' || name.front() == ' ' ||
      name.back() == '.' || name.back() == ' ' || name.size() <= 5 ||
      !equalIgnoreCase(name.substr(name.size() - 5), ".epub"))
    return false;
  // Avoid device names which cannot be copied safely through desktop FAT tools.
  const auto stem = trim(name.substr(0, name.find('.')));
  if (equalIgnoreCase(stem, "CON") || equalIgnoreCase(stem, "PRN") || equalIgnoreCase(stem, "AUX") ||
      equalIgnoreCase(stem, "NUL") ||
      (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
       (equalIgnoreCase(stem.substr(0, 3), "COM") || equalIgnoreCase(stem.substr(0, 3), "LPT"))))
    return false;
  for (size_t i = 0; i < name.size();) {
    const auto first = static_cast<uint8_t>(name[i++]);
    uint32_t cp = first;
    size_t continuation = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuation = 1;
      cp &= 0x1F;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuation = 2;
      cp &= 0x0F;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuation = 3;
      cp &= 0x07;
      minimum = 0x10000;
    } else if (first >= 0x80)
      return false;
    if (continuation > name.size() - i) return false;
    for (size_t j = 0; j < continuation; ++j) {
      const auto next = static_cast<uint8_t>(name[i++]);
      if ((next & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp < 32 || (cp >= 0x7F && cp <= 0x9F) ||
        cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|')
      return false;
  }
  return true;
}
inline bool decode(std::string_view value, bool extended, bool quoted, std::string& output) {
  if (extended) {
    const size_t first = value.find('\'');
    if (first == std::string_view::npos || !equalIgnoreCase(value.substr(0, first), "UTF-8")) return false;
    const size_t second = value.find('\'', first + 1);
    if (second == std::string_view::npos) return false;
    value.remove_prefix(second + 1);
  }
  // Bounded temporary fits the <=256-byte stack rule; only the final name is
  // copied to a cold-path string (at most 240 bytes plus its terminator).
  std::array<char, MAX_NAME_BYTES + 1> bytes{};
  size_t used = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (extended && c == '%') {
      if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0) return false;
      c = static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
      i += 2;
    } else if (quoted && c == '\\') {
      if (++i >= value.size()) return false;
      c = value[i];
    }
    if (used == MAX_NAME_BYTES) return false;
    bytes[used++] = c;
  }
  const std::string_view name(bytes.data(), used);
  if (!safe(name)) return false;
  output.assign(name);
  return true;
}
inline bool parse(std::string_view header, std::string& output) {
  output.clear();
  if (header.size() > MAX_HEADER_BYTES || header.find_first_of("\r\n") != std::string_view::npos ||
      header.find('\0') != std::string_view::npos)
    return false;
  size_t cursor = header.find(';');
  if (cursor == std::string_view::npos) return false;
  std::string_view regular, extended;
  bool hasRegular = false, hasExtended = false, regularQuoted = false;
  while (cursor < header.size()) {
    ++cursor;
    while (cursor < header.size() && (header[cursor] == ' ' || header[cursor] == '\t')) ++cursor;
    if (cursor == header.size()) break;
    const size_t keyStart = cursor;
    while (cursor < header.size() && header[cursor] != '=' && header[cursor] != ';') ++cursor;
    if (cursor == header.size() || header[cursor] != '=') return false;
    const auto key = trim(header.substr(keyStart, cursor - keyStart));
    ++cursor;
    while (cursor < header.size() && (header[cursor] == ' ' || header[cursor] == '\t')) ++cursor;
    const bool quoted = cursor < header.size() && header[cursor] == '"';
    if (quoted) ++cursor;
    const size_t start = cursor;
    if (quoted) {
      while (cursor < header.size() && header[cursor] != '"') {
        if (header[cursor] == '\\') ++cursor;
        if (cursor == header.size()) return false;
        ++cursor;
      }
      if (cursor == header.size()) return false;
    } else {
      while (cursor < header.size() && header[cursor] != ';') ++cursor;
    }
    const auto value = quoted ? header.substr(start, cursor - start) : trim(header.substr(start, cursor - start));
    if (quoted) {
      ++cursor;
      while (cursor < header.size() && (header[cursor] == ' ' || header[cursor] == '\t')) ++cursor;
      if (cursor < header.size() && header[cursor] != ';') return false;
    }
    if (equalIgnoreCase(key, "filename*")) {
      if (hasExtended || quoted) return false;
      hasExtended = true;
      extended = value;
    } else if (equalIgnoreCase(key, "filename")) {
      if (hasRegular) return false;
      hasRegular = true;
      regular = value;
      regularQuoted = quoted;
    }
  }
  if (hasExtended && decode(extended, true, false, output)) return true;
  return hasRegular && decode(regular, false, regularQuoted, output);
}
}  // namespace HttpDownloadFilename
