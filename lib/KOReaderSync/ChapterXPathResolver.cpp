#include "ChapterXPathResolver.h"

#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

// XML formatting whitespace; other Unicode spaces can be meaningful book content.
bool isWhitespaceOnly(const XML_Char* data, const int len) {
  for (int i = 0; i < len; ++i) {
    const char c = data[i];
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') return false;
  }
  return true;
}

int trailingWhitespace(const XML_Char* data, const int len) {
  int trailing = 0;
  while (trailing < len && isWhitespaceOnly(data + len - trailing - 1, 1)) ++trailing;
  return trailing;
}

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

bool equalsTag(const std::string_view name, const std::string_view tag) {
  if (name.size() != tag.size()) return false;
  for (size_t i = 0; i < name.size(); i++) {
    const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] + ('a' - 'A')) : name[i];
    if (c != tag[i]) return false;
  }
  return true;
}

bool isNonVisibleTextTag(const std::string_view name) {
  return equalsTag(name, "head") || equalsTag(name, "style") || equalsTag(name, "script") || equalsTag(name, "title") ||
         equalsTag(name, "rp") || equalsTag(name, "rt");
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  // Exclude trailing formatting whitespace so 100% ends in real text.
  size_t totalVisibleChars() const { return lastTextEndChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || isNonVisibleTextTag(name)) nonVisibleDepth++;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) nonVisibleDepth--;
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
    if (!isWhitespaceOnly(data, len)) {
      // XML whitespace is single-byte, so its byte and codepoint counts agree.
      lastTextEndChars = visibleChars - static_cast<size_t>(trailingWhitespace(data, len));
    }
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t lastTextEndChars = 0;
};

class XPathElementResolver final : public Print {
 public:
  XPathElementResolver(const int targetElement, const char* targetTag)
      : targetElement(targetElement), targetTag(targetTag) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathElementResolver::startElement, &XPathElementResolver::endElement);
  }

  ~XPathElementResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathElementResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathElementResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    if (name == targetTag) {
      elementCount++;
      if (elementCount == targetElement) {
        xpath = buildParagraphXPath(spineIndex, path, 0, 0);
        stopped = true;
        XML_StopParser(parser, XML_FALSE);
      }
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetElement;
  const char* targetTag;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int elementCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  enum class BoundaryMode { Exclusive, Inclusive };

  explicit XPathProgressResolver(const size_t targetVisibleChar,
                                 const BoundaryMode boundaryMode = BoundaryMode::Exclusive,
                                 const uint8_t minimumPathDepth = 0)
      : targetVisibleChar(targetVisibleChar), boundaryMode(boundaryMode), minimumPathDepth(minimumPathDepth) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    XML_SetCommentHandler(parser, &XPathProgressResolver::comment);
    XML_SetProcessingInstructionHandler(parser, &XPathProgressResolver::processingInstruction);
    XML_SetCdataSectionHandler(parser, &XPathProgressResolver::startCdataSection,
                               &XPathProgressResolver::endCdataSection);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t getLastEligibleEnd() const { return lastEligibleEnd; }
  size_t getVisibleChars() const { return visibleChars; }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  static void XMLCALL comment(void* userData, const XML_Char*) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  static void XMLCALL processingInstruction(void* userData, const XML_Char*, const XML_Char*) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  static void XMLCALL startCdataSection(void* userData) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  static void XMLCALL endCdataSection(void* userData) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        textNodeIndexStack.push_back(0);
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (nonVisibleDepth > 0 || isNonVisibleTextTag(name)) nonVisibleDepth++;

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) nonVisibleDepth--;

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    pendingTextNode = true;
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    const bool whitespaceOnly = isWhitespaceOnly(data, len);

    // Start a new text node on first non-empty content after any structural boundary.
    // Only counting non-empty nodes matches KOReader's text()[N] indexing behavior,
    // which skips empty text nodes created by bare <a id="anchor"/> anchors.
    // Keep whitespace-only nodes in the numbering: crengine's block whitespace
    // rules depend on computed CSS, which this streaming resolver cannot see.
    if (pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    const bool targetInCurrentChunk = boundaryMode == BoundaryMode::Inclusive ? targetVisibleChar <= nextVisibleChars
                                                                              : targetVisibleChar < nextVisibleChars;
    const bool eligiblePath = path.size() >= minimumPathDepth;
    if (eligiblePath && !whitespaceOnly) {
      lastEligibleEnd = nextVisibleChars - static_cast<size_t>(trailingWhitespace(data, len));
    }
    if (targetInCurrentChunk && eligiblePath && !whitespaceOnly) {
      // Advance whitespace targets to real text independently of Expat's chunk
      // boundaries. Inclusive progress may still end just after a real character.
      const size_t delta = targetVisibleChar > visibleChars ? targetVisibleChar - visibleChars : 0;
      const auto* ptr = reinterpret_cast<const unsigned char*>(data);
      const auto* end = ptr + len;
      size_t offset = 0;
      while (ptr < end) {
        const bool whitespace = isWhitespaceOnly(reinterpret_cast<const XML_Char*>(ptr), 1);
        const bool reached = boundaryMode == BoundaryMode::Inclusive ? delta <= offset + 1 : delta <= offset;
        if (reached && !whitespace) {
          const int textNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
          const size_t charOff = visibleChars - textNodeStartChars + std::max(delta, offset);
          xpath = buildParagraphXPath(spineIndex, path, textNode, charOff);
          stopped = true;
          XML_StopParser(parser, XML_FALSE);
          return;
        }
        utf8NextCodepoint(&ptr);
        ++offset;
      }
    }

    visibleChars = nextVisibleChars;
  }

  void onMarkupBoundary() {
    if (!insideBody || nonVisibleDepth > 0 || stopped || pendingTextNode) return;
    pendingTextNode = true;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  const BoundaryMode boundaryMode;
  const uint8_t minimumPathDepth;
  size_t lastEligibleEnd = 0;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

std::string resolveVisibleOffset(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& href,
                                 size_t target, XPathProgressResolver::BoundaryMode boundaryMode,
                                 const uint8_t minimumPathDepth) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    XPathProgressResolver resolver(target, boundaryMode, minimumPathDepth);
    if (!resolver.ok()) return "";
    resolver.spineIndex = spineIndex;
    if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) return "";
    if (resolver.hasMatch()) return resolver.getXPath();
    if (minimumPathDepth == 0 || resolver.getLastEligibleEnd() == 0 || target > resolver.getVisibleChars()) return "";
    // Trailing container whitespace has no next child. Re-read at the last
    // eligible character, without retaining a path for every parsed text chunk.
    target = resolver.getLastEligibleEnd() - 1;
    boundaryMode = XPathProgressResolver::BoundaryMode::Exclusive;
  }
  return "";
}

std::string findXPathForElement(const std::shared_ptr<Epub>& epub, const int spineIndex, const uint16_t elementIndex,
                                const char* tagName) {
  if (!epub || elementIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathElementResolver resolver(elementIndex, tagName);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved %s %u in spine %d -> %s", tagName, elementIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "%s %u not found in spine %d", tagName, elementIndex, spineIndex);
  return "";
}
}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  return findXPathForElement(epub, spineIndex, paragraphIndex, "p");
}

std::string ChapterXPathResolver::findXPathForListItem(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const uint16_t listItemIndex) {
  return findXPathForElement(epub, spineIndex, listItemIndex, "li");
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress, const uint8_t minimumPathDepth) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    if (minimumPathDepth > 0) return findXPathForVisibleTextOffset(epub, spineIndex, 0, minimumPathDepth);
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  size_t totalVisibleChars = 0;
  {
    ParagraphTextCounter counter;
    if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
      return "";
    }
    totalVisibleChars = counter.totalVisibleChars();
  }
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  const auto xpath = resolveVisibleOffset(epub, spineIndex, href, targetVisibleChar,
                                          XPathProgressResolver::BoundaryMode::Inclusive, minimumPathDepth);
  if (xpath.empty()) {
    LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  } else {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex, xpath.c_str());
  }
  return xpath;
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset,
                                                                const uint8_t minimumPathDepth) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  const auto xpath = resolveVisibleOffset(epub, spineIndex, href, visibleTextOffset,
                                          XPathProgressResolver::BoundaryMode::Exclusive, minimumPathDepth);
  if (xpath.empty()) {
    LOG_DBG("KOX", "Could not resolve visible offset %lu in spine %d", static_cast<unsigned long>(visibleTextOffset),
            spineIndex);
  }
  return xpath;
}
