#include <FsHelpers.h>
#include <Logging.h>

#include "Epub.h"
#include "Epub/parsers/AdobeFontObfuscation.h"

bool Epub::readFontContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  {
    size_t size = 0;
    if (!getItemSize("META-INF/encryption.xml", &size)) return readItemContentsToStream(path, out, chunkSize);
    AdobeFontObfuscation manifest(&path);
    if (!manifest.setup() || !readItemContentsToStream("META-INF/encryption.xml", manifest, 512) ||
        !manifest.finish()) {
      LOG_ERR("EBP", "Cannot read font obfuscation manifest");
      return false;
    }
    if (!manifest.isAdobeFont()) return readItemContentsToStream(path, out, chunkSize);
  }

  AdobeFontObfuscation::Key key{};
  {
    // Read independently of the metadata cache so previously opened books also
    // work, without adding persistent UUID state or changing the cache format.
    std::string opfPath;
    if (!findContentOpfFile(&opfPath)) return false;
    AdobeFontObfuscation package;
    if (!package.setup() || !readItemContentsToStream(opfPath, package, 512) || !package.finish() ||
        !package.hasKey()) {
      LOG_ERR("EBP", "Cannot find UUID for Adobe-obfuscated font: %s", path.c_str());
      return false;
    }
    key = package.key();
  }
  AdobeFontStream font(out, key);
  return readItemContentsToStream(path, font, chunkSize);
}
