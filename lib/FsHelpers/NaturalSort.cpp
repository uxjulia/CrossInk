#include "NaturalSort.h"

#include <cctype>

namespace FsHelpers {

int naturalCompare(const char* s1, const char* s2) {
  while (*s1 && *s2) {
    if (isdigit(static_cast<unsigned char>(*s1)) && isdigit(static_cast<unsigned char>(*s2))) {
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;

      int len1 = 0;
      int len2 = 0;
      while (isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
      while (isdigit(static_cast<unsigned char>(s2[len2]))) len2++;

      if (len1 != len2) return len1 - len2;

      for (int i = 0; i < len1; i++) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
      }

      s1 += len1;
      s2 += len2;
    } else {
      const int c1 = tolower(static_cast<unsigned char>(*s1));
      const int c2 = tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 - c2;
      s1++;
      s2++;
    }
  }

  if (*s1 == *s2) return 0;
  return (*s1 == '\0') ? -1 : 1;
}

size_t naturalSortKey(const char* name, uint8_t* out, size_t cap) {
  static constexpr uint8_t DIGIT_RUN_MARKER = 0x30;
  size_t n = 0;
  const char* s = name;

  while (*s && n < cap) {
    if (isdigit(static_cast<unsigned char>(*s))) {
      while (*s == '0') s++;

      size_t len = 0;
      while (isdigit(static_cast<unsigned char>(s[len]))) len++;

      out[n++] = DIGIT_RUN_MARKER;
      if (n >= cap) break;

      if (len == 0) {
        out[n++] = 1;
        if (n >= cap) break;
        out[n++] = '0';
      } else {
        out[n++] = static_cast<uint8_t>(len > 255 ? 255 : len);
        for (size_t i = 0; i < len && n < cap; i++) {
          out[n++] = static_cast<uint8_t>(s[i]);
        }
      }
      s += len;
    } else {
      out[n++] = static_cast<uint8_t>(tolower(static_cast<unsigned char>(*s)));
      s++;
    }
  }

  return n;
}

}  // namespace FsHelpers
