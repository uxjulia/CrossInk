#include <FontFileTypes.h>
#include <HalStorage.h>
#include <Logging.h>

#include "FontInstaller.h"
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif
#include <cctype>
#include <cstring>

namespace {
bool isSafeFontPathChar(const char c) {
  return static_cast<unsigned char>(c) >= 0x80 || std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
         c == ' ' || c == '.' || c == '(' || c == ')';
}
}  // namespace

bool FontInstaller::isValidCpfontFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path separators / traversal up front. Anything that could escape
  // the family directory or refer to a different one is a hard reject.
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

#if CROSSINK_SCALABLE_FONTS
  if (fontfiles::isScalable(name)) {
    for (const char* p = name; *p; ++p)
      if (!isSafeFontPathChar(*p)) return false;
    return true;
  }
#endif
  // Must end with ".cpfont" exactly.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  size_t nameLen = strlen(name);
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;

  // Basename (before .cpfont) must stay within safe single-path-component
  // characters. Additional dots are allowed because GitHub release assets
  // expose spaces as dots.
  size_t baseLen = nameLen - kExtLen;
  for (size_t i = 0; i < baseLen; ++i) {
    char c = name[i];
    if (!isSafeFontPathChar(c)) {
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  const bool scalable = fontfiles::isScalable(path);
  uint8_t magic[CPFONT_MAGIC_LEN];
  const int bytesRead = file.read(magic, CPFONT_MAGIC_LEN);
  file.close();

  if (bytesRead != static_cast<int>(CPFONT_MAGIC_LEN)) {
    LOG_ERR("FONT", "File too small: %s (%d bytes)", path, bytesRead);
    return false;
  }

#if CROSSINK_SCALABLE_FONTS
  if (scalable) {
    HalScalableFont::Info info;
    return HalScalableFont::inspectFile(path, info);
  }
#endif
  if (scalable || memcmp(magic, "CPFONT\0\0", CPFONT_MAGIC_LEN) != 0) {
    LOG_ERR("FONT", "Bad magic in: %s", path);
    return false;
  }

  return true;
}

bool FontInstaller::finishUpload(const char* path, bool writesOk) {
  const bool valid = writesOk && validateCpfontFile(path);
  if (!valid && path && *path && !Storage.remove(path)) LOG_ERR("FONT", "Cannot remove failed font upload: %s", path);
  return valid;
}
