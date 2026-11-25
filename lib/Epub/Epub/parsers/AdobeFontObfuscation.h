#pragma once

#include <Print.h>
#include <XmlParserUtils.h>

#include <array>
#include <string>

// Reads only the requested font's manifest entry, or the first UUID identifier
// in package metadata. No manifest/resource list is retained in RAM.
class AdobeFontObfuscation : public Print {
 public:
  using Key = std::array<uint8_t, 16>;
  explicit AdobeFontObfuscation(const std::string* fontPath = nullptr) : fontPath_(fontPath) {}
  ~AdobeFontObfuscation() override { destroyXmlParser(parser_); }
  bool setup();
  bool finish();
  bool isAdobeFont() const { return adobeFont_; }
  bool hasKey() const { return hasKey_; }
  const Key& key() const { return key_; }
  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, size_t size) override;

 private:
  XML_Parser parser_ = nullptr;
  const std::string* fontPath_;
  Key key_{};
  char identifier_[46]{};
  size_t identifierLength_ = 0;
  unsigned depth_ = 0;
  bool rootValid_ = false;
  bool metadata_ = false;
  bool identifierActive_ = false;
  bool uuidScheme_ = false;
  bool identifierOverflow_ = false;
  bool hasKey_ = false;
  bool encryptedData_ = false;
  bool cipherData_ = false;
  bool adobeMethod_ = false;
  bool matchingPath_ = false;
  bool adobeFont_ = false;
  static void XMLCALL start(void* context, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL end(void* context, const XML_Char* name);
  static void XMLCALL text(void* context, const XML_Char* data, int size);
};

// Transform only the prefix, with an offset that survives arbitrary ZIP chunks.
// The input belongs to the ZIP reader and must never be modified in place.
class AdobeFontStream : public Print {
 public:
  AdobeFontStream(Print& output, const AdobeFontObfuscation::Key& key) : output_(output), key_(key) {}
  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, size_t size) override;

 private:
  Print& output_;
  const AdobeFontObfuscation::Key key_;
  size_t offset_ = 0;
};
