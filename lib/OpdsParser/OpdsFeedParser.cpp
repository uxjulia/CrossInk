#include "OpdsFeedParser.h"

#include <Logging.h>

#include <cstring>

void OpdsFeedParser::setPreferredLanguage(const char* value) {
  if (!value) return;
  const auto length = strnlen(value, sizeof(language) - 1);
  memcpy(language, value, length);
  language[length] = '\0';
}

size_t OpdsFeedParser::write(const uint8_t* bytes, size_t length) {
  if (failure != OpdsParserError::NONE || (!bytes && length)) return 0;
  size_t start = 0;
  while (!xml && !json && start < length) {
    const auto byte = bytes[start];
    if (bomBytes) {
      if (byte != (bomBytes == 1 ? 0xBB : 0xBF)) {
        failure = OpdsParserError::INVALID_INPUT;
        return 0;
      }
      bomBytes = bomBytes == 1 ? 2 : 0;
      ++start;
      continue;
    }
    if (byte == 0xEF) {
      bomBytes = 1;
      ++start;
      continue;
    }
    if (byte == ' ' || byte == '\r' || byte == '\n' || byte == '\t') {
      ++start;
      continue;
    }
    if (byte == '<')
      xml = makeUniqueNoThrow<OpdsParser>(entries, capacity);
    else if (byte == '{') {
      json = makeUniqueNoThrow<OpdsJsonParser>(entries, capacity);
      if (json) json->setPreferredLanguage(language);
    } else {
      failure = OpdsParserError::INVALID_INPUT;
      return 0;
    }
    if (!xml && !json) {
      LOG_ERR("OPDS", "Unable to allocate feed parser");
      failure = OpdsParserError::PARSER_MEMORY;
      return 0;
    }
  }
  Print* parser = xml ? static_cast<Print*>(xml.get()) : static_cast<Print*>(json.get());
  return !parser || parser->write(bytes + start, length - start) == length - start ? length : 0;
}

void OpdsFeedParser::flush() {
  if (xml)
    xml->flush();
  else if (json)
    json->flush();
  else
    failure = OpdsParserError::INVALID_INPUT;
}

OpdsParserError OpdsFeedParser::getErrorReason() const {
  if (failure != OpdsParserError::NONE) return failure;
  if (xml) return xml->getErrorReason();
  return json && json->error() ? OpdsParserError::INVALID_INPUT : OpdsParserError::NONE;
}

const std::string& OpdsFeedParser::getSearchTemplate() const {
  static const std::string empty;
  return xml ? xml->getSearchTemplate() : json ? json->getSearchTemplate() : empty;
}
const std::string& OpdsFeedParser::getNextPageUrl() const {
  static const std::string empty;
  return xml ? xml->getNextPageUrl() : json ? json->getNextPageUrl() : empty;
}
const std::string& OpdsFeedParser::getPrevPageUrl() const {
  static const std::string empty;
  return xml ? xml->getPrevPageUrl() : json ? json->getPrevPageUrl() : empty;
}
