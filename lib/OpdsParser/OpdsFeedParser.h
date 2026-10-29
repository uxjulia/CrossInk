#pragma once

#include <Memory.h>

#include <memory>

#include "OpdsJsonParser.h"

// Sniff the response, then allocate only the selected parser. Both formats
// write into the browser's existing bounded, fallibly allocated entry array.
class OpdsFeedParser final : public Print {
 public:
  OpdsFeedParser(OpdsEntry* entries, size_t capacity) : entries(entries), capacity(capacity) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override;
  void flush() override;
  explicit operator bool() const { return getErrorReason() == OpdsParserError::NONE; }
  OpdsParserError getErrorReason() const;
  size_t getEntryCount() const { return xml ? xml->getEntryCount() : json ? json->getEntryCount() : 0; }
  bool wasTruncated() const { return xml ? xml->wasTruncated() : json && json->truncated(); }
  const std::string& getSearchTemplate() const;
  const std::string& getNextPageUrl() const;
  const std::string& getPrevPageUrl() const;
  void setPreferredLanguage(const char* language);

 private:
  OpdsEntry* entries;
  size_t capacity;
  std::unique_ptr<OpdsParser> xml;
  std::unique_ptr<OpdsJsonParser> json;
  OpdsParserError failure = OpdsParserError::NONE;
  uint8_t bomBytes = 0;
  char language[12] = {};
};
