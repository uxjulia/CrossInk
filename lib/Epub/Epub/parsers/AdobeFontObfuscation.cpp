#include "AdobeFontObfuscation.h"

#include <FsHelpers.h>
#include <strings.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace {
constexpr char Opf[] = "http://www.idpf.org/2007/opf";
constexpr char Dc[] = "http://purl.org/dc/elements/1.1/";
constexpr char Enc[] = "http://www.w3.org/2001/04/xmlenc#";

std::string fontReference(const char* uri) {
  auto path = FsHelpers::decodeUriEscapes(uri);
  // The shared ZIP helper resolves '..' but retains '.'. Remove single-dot
  // segments first so both './font.ttf' and 'dir/./../font.ttf' resolve correctly.
  for (size_t start = 0; start < path.size();) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == std::string::npos ? path.size() : slash;
    if (end == start + 1 && path[start] == '.')
      path.erase(start, 1 + (slash != std::string::npos));
    else
      start = end + 1;
  }
  return FsHelpers::normalisePath(path);
}

bool element(const char* name, const char* ns, const char* local, bool allowBare = true) {
  const size_t length = std::strlen(ns);
  return (std::strncmp(name, ns, length) == 0 && name[length] == '|' && std::strcmp(name + length + 1, local) == 0) ||
         (allowBare && std::strcmp(name, local) == 0);
}

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool uuidKey(const char* text, bool scheme, AdobeFontObfuscation::Key& key) {
  if (strncasecmp(text, "urn:uuid:", 9) == 0)
    text += 9;
  else if (!scheme)
    return false;
  const size_t length = std::strlen(text);
  if (length != 36 && length != 32) return false;
  size_t pos = 0;
  for (size_t i = 0; i < key.size(); ++i) {
    if (length == 36 && (i == 4 || i == 6 || i == 8 || i == 10) && text[pos++] != '-') return false;
    const int high = hex(text[pos++]), low = hex(text[pos++]);
    if (high < 0 || low < 0) return false;
    key[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}
}  // namespace

bool AdobeFontObfuscation::setup() {
  parser_ = XML_ParserCreateNS(nullptr, '|');
  if (!parser_) return false;
  XML_SetUserData(parser_, this);
  XML_SetElementHandler(parser_, start, end);
  XML_SetCharacterDataHandler(parser_, text);
  // Package/manifest metadata does not need a DTD or entity expansion.
  XML_SetStartDoctypeDeclHandler(parser_, [](void* context, const char*, const char*, const char*, int) {
    XML_StopParser(static_cast<AdobeFontObfuscation*>(context)->parser_, XML_FALSE);
  });
  return true;
}

bool AdobeFontObfuscation::finish() { return parser_ && XML_Parse(parser_, "", 0, XML_TRUE) == XML_STATUS_OK; }

size_t AdobeFontObfuscation::write(const uint8_t* data, size_t size) {
  if (!parser_ || size > INT_MAX ||
      XML_Parse(parser_, reinterpret_cast<const char*>(data), static_cast<int>(size), XML_FALSE) == XML_STATUS_ERROR)
    return 0;
  return size;
}

void XMLCALL AdobeFontObfuscation::start(void* context, const XML_Char* name, const XML_Char** atts) {
  auto& self = *static_cast<AdobeFontObfuscation*>(context);
  ++self.depth_;
  if (self.depth_ == 1) {
    self.rootValid_ = self.fontPath_ ? element(name, "urn:oasis:names:tc:opendocument:xmlns:container", "encryption")
                                     : element(name, Opf, "package");
  }
  if (!self.rootValid_) return;
  if (self.fontPath_) {
    if (self.depth_ == 2 && element(name, Enc, "EncryptedData")) {
      self.encryptedData_ = true;
      self.adobeMethod_ = self.matchingPath_ = false;
    } else if (self.encryptedData_ && self.depth_ == 3 && element(name, Enc, "EncryptionMethod")) {
      for (size_t i = 0; atts[i]; i += 2)
        if (std::strcmp(atts[i], "Algorithm") == 0)
          self.adobeMethod_ = std::strcmp(atts[i + 1], "http://ns.adobe.com/pdf/enc#RC") == 0;
    } else if (self.encryptedData_ && self.depth_ == 3 && element(name, Enc, "CipherData")) {
      self.cipherData_ = true;
    } else if (self.cipherData_ && self.depth_ == 4 && element(name, Enc, "CipherReference")) {
      for (size_t i = 0; atts[i]; i += 2) {
        // Bound the temporary path allocation; paths elsewhere in the EPUB
        // loader use the same URI decoding and ZIP-root normalization.
        if (std::strcmp(atts[i], "URI") == 0 && std::strlen(atts[i + 1]) <= 2048)
          self.matchingPath_ = fontReference(atts[i + 1]) == *self.fontPath_;
      }
    }
  } else {
    if (self.depth_ == 2 && element(name, Opf, "metadata")) self.metadata_ = true;
    if (self.metadata_ && self.depth_ == 3 && element(name, Dc, "identifier", false) && !self.hasKey_) {
      self.identifierActive_ = true;
      self.identifierLength_ = 0;
      self.identifierOverflow_ = self.uuidScheme_ = false;
      for (size_t i = 0; atts[i]; i += 2)
        if (element(atts[i], Opf, "scheme") && strcasecmp(atts[i + 1], "uuid") == 0) self.uuidScheme_ = true;
    }
  }
}

void XMLCALL AdobeFontObfuscation::end(void* context, const XML_Char* name) {
  auto& self = *static_cast<AdobeFontObfuscation*>(context);
  if (self.fontPath_) {
    if (self.depth_ == 2 && self.encryptedData_ && element(name, Enc, "EncryptedData")) {
      self.adobeFont_ |= self.adobeMethod_ && self.matchingPath_;
      self.encryptedData_ = false;
    }
    if (self.depth_ == 3 && element(name, Enc, "CipherData")) self.cipherData_ = false;
  } else {
    if (self.depth_ == 3 && self.identifierActive_) {
      self.identifier_[self.identifierLength_] = '\0';
      self.hasKey_ = !self.identifierOverflow_ && uuidKey(self.identifier_, self.uuidScheme_, self.key_);
      self.identifierActive_ = false;
    }
    if (self.depth_ == 2 && element(name, Opf, "metadata")) self.metadata_ = false;
  }
  --self.depth_;
}

void XMLCALL AdobeFontObfuscation::text(void* context, const XML_Char* data, int size) {
  auto& self = *static_cast<AdobeFontObfuscation*>(context);
  if (!self.identifierActive_ || self.depth_ != 3) return;
  for (int i = 0; i < size; ++i) {
    const char c = data[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    if (self.identifierLength_ < sizeof(self.identifier_) - 1)
      self.identifier_[self.identifierLength_++] = c;
    else
      self.identifierOverflow_ = true;
  }
}

size_t AdobeFontStream::write(const uint8_t* data, size_t size) {
  size_t written = 0;
  uint8_t buffer[128];
  while (written < size && offset_ < 1024) {
    const size_t count = std::min({size - written, sizeof(buffer), 1024 - offset_});
    for (size_t i = 0; i < count; ++i) buffer[i] = data[written + i] ^ key_[(offset_ + i) % key_.size()];
    const size_t accepted = output_.write(buffer, count);
    written += accepted;
    offset_ += accepted;
    if (accepted != count) return written;
  }
  if (written < size) written += output_.write(data + written, size - written);
  return written;
}
