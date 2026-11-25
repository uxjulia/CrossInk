#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstring>
#include <utility>

namespace {
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;
// Only short "<N> books" summaries carry a count; one char past the limit
// marks a longer (descriptive) summary as not-a-count.
constexpr size_t MAX_SUMMARY_COUNT_CHARS = 32;

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit(const char c) { return c >= '0' && c <= '9'; }

// Consumes a non-negative decimal integer at `p`; false on no digits or overflow.
bool consumeCount(const char*& p, int32_t& out) {
  if (!isDigit(*p)) return false;
  int32_t value = 0;
  for (; isDigit(*p); ++p) {
    if (value > (INT32_MAX - (*p - '0')) / 10) return false;
    value = value * 10 + (*p - '0');
  }
  out = value;
  return true;
}

// thr:count attribute value: digits only; -1 otherwise.
int32_t parseAttributeCount(const char* text) {
  int32_t value = -1;
  if (!text || !consumeCount(text, value) || *text != '\0') return -1;
  return value;
}

// "<N> book(s)" summaries such as Mayberry's "12713 books"; a descriptive
// sentence that happens to start with a number yields -1.
int32_t parseSummaryCount(const std::string& summary) {
  if (summary.size() > MAX_SUMMARY_COUNT_CHARS) return -1;
  const char* p = summary.c_str();
  while (isSpace(*p)) ++p;
  int32_t value = -1;
  if (!consumeCount(p, value) || (*p != ' ' && *p != '\t')) return -1;
  while (isSpace(*p)) ++p;
  // Only the known catalog summary form; arbitrary "N words" may be a description.
  if (strncmp(p, "books", 5) == 0) {
    p += 5;
  } else if (strncmp(p, "book", 4) == 0) {
    p += 4;
  } else {
    return -1;
  }
  while (isSpace(*p)) ++p;
  return *p == '\0' ? value : -1;
}
const char* localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

void storeDescription(std::array<char, MAX_OPDS_DESCRIPTION_BYTES + 1>& out, std::string& text) {
  while (!text.empty() && isSpace(text.back())) text.pop_back();
  size_t length = text.size();
  // The byte cap may split a UTF-8 codepoint. Remove only that partial suffix.
  if (length) {
    size_t start = length - 1;
    while (start && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
    const auto c = static_cast<unsigned char>(text[start]);
    const size_t bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (length - start < bytes) length = start;
  }
  memcpy(out.data(), text.data(), length);
  out[length] = '\0';
  // Make a bounded preview visibly different from a complete short summary.
  if (text.size() == MAX_OPDS_DESCRIPTION_BYTES && length >= 3) {
    while (length > MAX_OPDS_DESCRIPTION_BYTES - 3) {
      --length;
      while (length && (static_cast<unsigned char>(out[length]) & 0xC0) == 0x80) --length;
    }
    memcpy(out.data() + length, "...", 4);
  }
}

}  // namespace

OpdsParser::OpdsParser(OpdsEntry* entries, const size_t entryCapacity)
    : entries(entries), entryCapacity(entryCapacity) {
  if (!entries || entryCapacity == 0) {
    errorOccured = true;
    errorReason = OpdsParserError::NO_ENTRY_BUFFER;
    LOG_DBG("OPDS", "No entry buffer supplied");
  }

  resetXmlParser();
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured || !parser) {
    errorOccured = true;
    return 0;
  }
  if (!xmlData && length > 0) {
    errorOccured = true;
    errorReason = OpdsParserError::INVALID_INPUT;
    return 0;
  }

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      errorReason = OpdsParserError::BUFFER_MEMORY;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      parser = nullptr;
      return 0;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      errorReason = OpdsParserError::XML_PARSE;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      parser = nullptr;
      return 0;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (!parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    errorReason = OpdsParserError::XML_PARSE;
    destroyXmlParser(parser);
    parser = nullptr;
  }
}

bool OpdsParser::parse(const uint8_t* xmlData, const size_t length) {
  clear();
  if (!xmlData && length > 0) {
    errorOccured = true;
    errorReason = OpdsParserError::INVALID_INPUT;
    return false;
  }

  if (length > 0) {
    write(xmlData, length);
  }
  flush();
  return !error();
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entryCount = 0;
  truncated = false;
  searchTemplate.clear();
  searchDescriptionUrl.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inSummary = false;
  summaryIsContent = descriptionMarkup = descriptionIsHtml = collectDescription = false;
  descriptionDepth = 0;
  errorOccured = !entries || entryCapacity == 0;
  errorReason = errorOccured ? OpdsParserError::NO_ENTRY_BUFFER : OpdsParserError::NONE;
  resetXmlParser();
}

bool OpdsParser::resetXmlParser() {
  if (parser) {
    if (XML_ParserReset(parser, nullptr) != XML_TRUE) {
      destroyXmlParser(parser);
    }
  }

  if (!parser) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      errorOccured = true;
      errorReason = OpdsParserError::PARSER_MEMORY;
      LOG_DBG("OPDS", "Couldn't allocate memory for parser");
      return false;
    }
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::assignBounded(std::string& target, const char* value, const size_t maxLen) {
  if (!value) {
    target.clear();
    return;
  }
  target.assign(value, strnlen(value, maxLen));
}

void OpdsParser::appendBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  if (target.size() >= maxLen) return;
  const size_t remaining = maxLen - target.size();
  target.append(value, len < remaining ? len : remaining);
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (self->inSummary) {
    ++self->descriptionDepth;
    if (!self->currentText.empty() && !isSpace(self->currentText.back()) &&
        self->currentText.size() < MAX_OPDS_DESCRIPTION_BYTES)
      self->currentText += ' ';
    return;  // XHTML descendants must never overwrite Atom title/author/link fields.
  }

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0 && !self->inEntry) {
        if (strstr(href, "{searchTerms}") != nullptr) {
          assignBounded(self->searchTemplate, href, MAX_SEARCH_TEMPLATE_CHARS);
        } else if (type && std::strcmp(type, "application/opensearchdescription+xml") == 0 &&
                   strnlen(href, MAX_SEARCH_TEMPLATE_CHARS + 1) <= MAX_SEARCH_TEMPLATE_CHARS) {
          self->searchDescriptionUrl = href;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        assignBounded(self->nextPageUrl, href, MAX_PAGE_URL_CHARS);
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        assignBounded(self->prevPageUrl, href, MAX_PAGE_URL_CHARS);
      }

      if (self->inEntry) {
        if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
            strcmp(type, "application/epub+zip") == 0) {
          // Prefer plain EPUB links over derived formats when multiple
          // acquisition links are present for one entry.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            assignBounded(self->currentEntry.href, href, MAX_HREF_CHARS);
            // Atom threading extension: thr:count="N" (any namespace prefix).
            for (int i = 0; atts[i]; i += 2) {
              const char* colon = strrchr(atts[i], ':');
              if (colon && strcmp(colon + 1, "count") == 0) {
                const int32_t count = parseAttributeCount(atts[i + 1]);
                if (count >= 0) self->currentEntry.count = count;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    self->inTitle = self->inAuthor = self->inAuthorName = self->inId = false;
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(localName(name), "summary") == 0 || strcmp(localName(name), "content") == 0) {
    self->inSummary = true;
    self->summaryIsContent = strcmp(localName(name), "content") == 0;
    self->descriptionDepth = 0;
    self->descriptionMarkup = false;
    const char* type = findAttribute(atts, "type");
    self->collectDescription =
        !findAttribute(atts, "src") &&
        (!type || strcmp(type, "text") == 0 || strcmp(type, "text/plain") == 0 || strcmp(type, "html") == 0 ||
         strcmp(type, "text/html") == 0 || strcmp(type, "xhtml") == 0);
    self->descriptionIsHtml = type && (strcmp(type, "html") == 0 || strcmp(type, "text/html") == 0);
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inSummary) {
    if (self->descriptionDepth) {
      --self->descriptionDepth;
      if (!self->currentText.empty() && !isSpace(self->currentText.back()) &&
          self->currentText.size() < MAX_OPDS_DESCRIPTION_BYTES)
        self->currentText += ' ';
      return;
    }
    if (!self->summaryIsContent && self->currentEntry.count < 0) {
      self->currentEntry.count = parseSummaryCount(self->currentText);
    }
    if (!self->currentText.empty() && (self->summaryIsContent || !self->currentEntry.description[0])) {
      storeDescription(self->currentEntry.description, self->currentText);
    }
    self->inSummary = false;
    return;
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      if (self->currentEntry.type != OpdsEntryType::NAVIGATION)
        self->currentEntry.count = -1;
      else
        self->currentEntry.description[0] = '\0';
      if (self->entryCount < self->entryCapacity) {
        self->entries[self->entryCount++] = std::move(self->currentEntry);
      } else {
        self->truncated = true;
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    // Retain the shared text scratch: moving it transfers a previous long
    // description's spare capacity into even a short title/author/id.
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      if (self->currentEntry.author.empty()) self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle) {
    appendBounded(self->currentText, s, len, MAX_TITLE_CHARS);
  } else if (self->inAuthorName) {
    appendBounded(self->currentText, s, len, MAX_AUTHOR_CHARS);
  } else if (self->inId) {
    appendBounded(self->currentText, s, len, MAX_ID_CHARS);
  } else if (self->inSummary && self->collectDescription) {
    std::string& text = self->currentText;
    for (int i = 0; i < len && text.size() < MAX_OPDS_DESCRIPTION_BYTES; ++i) {
      const char c = s[i];
      if (self->descriptionIsHtml && c == '<') {
        self->descriptionMarkup = true;
        if (!text.empty() && !isSpace(text.back())) text += ' ';
        continue;
      }
      if (self->descriptionMarkup) {
        if (c == '>') self->descriptionMarkup = false;
        continue;
      }
      if (isSpace(c) && (text.empty() || isSpace(text.back()))) continue;
      text += isSpace(c) ? ' ' : c;
    }
  }
}
