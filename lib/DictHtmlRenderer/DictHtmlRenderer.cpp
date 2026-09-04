#include "DictHtmlRenderer.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Tag classification
// ---------------------------------------------------------------------------

namespace {

char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

bool rangeTagEquals(const char* start, int len, const char* expected) {
  int i = 0;
  while (i < len && expected[i]) {
    if (asciiLower(start[i]) != expected[i]) return false;
    i++;
  }
  return i == len && expected[i] == '\0';
}

bool isDiscardedVoidTag(const char* name, int len) {
  static constexpr const char* kTags[] = {"area", "base", "col",   "embed",  "img",   "input",
                                          "link", "meta", "param", "source", "track", "wbr"};
  for (const char* tag : kTags) {
    if (rangeTagEquals(name, len, tag)) return true;
  }
  return false;
}

bool isDictionaryBullet(uint32_t cp) {
  // These geometric bullets are outside the default SD-font interval. Use the
  // guaranteed U+2022 glyph so dictionary definitions render consistently.
  switch (cp) {
    case 0x25A0:  // BLACK SQUARE
    case 0x25AA:  // BLACK SMALL SQUARE
    case 0x25C6:  // BLACK DIAMOND
    case 0x25CF:  // BLACK CIRCLE
      return true;
    default:
      return false;
  }
}

int encodeUtf8(uint32_t cp, char out[4]) {
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

}  // namespace

DictHtmlRenderer::TagAction DictHtmlRenderer::classify(const XML_Char* name) {
  // HTML tag names are ASCII case-insensitive. Expat deliberately preserves
  // source case, so normalize only the small tag name used for classification.
  char normalizedName[48];
  size_t nameLen = strlen(name);
  if (nameLen >= sizeof(normalizedName)) return TagAction::STRIP_KEEP;
  for (size_t i = 0; i <= nameLen; i++) normalizedName[i] = asciiLower(name[i]);
  name = normalizedName;

  // Block-strip: entire subtree discarded
  if (strcmp(name, "svg") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "hiero") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "math") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "gallery") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "nowiki") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "poem") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "ref") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "img") == 0) return TagAction::BLOCK_STRIP;

  // Children of block-strip tags (registered to avoid unknown-tag errors)
  if (strcmp(name, "defs") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "g") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "path") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "rect") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "use") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "table") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "tr") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "td") == 0) return TagAction::REGISTERED;

  // Inline formatting
  if (strcmp(name, "b") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "strong") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "i") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "em") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "u") == 0) return TagAction::FORMAT_UNDERLINE;
  if (strcmp(name, "s") == 0) return TagAction::FORMAT_STRIKE;
  if (strcmp(name, "sup") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "sub") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "code") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "tt") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "small") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "big") == 0) return TagAction::FORMAT_SMALL;

  // Block structure
  if (strcmp(name, "p") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "div") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "br") == 0 || strcmp(name, "hr") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "blockquote") == 0) return TagAction::BLOCK_QUOTE;

  // Lists
  if (strcmp(name, "li") == 0) return TagAction::LIST_ITEM;
  if (strcmp(name, "ol") == 0) return TagAction::LIST_CONTAINER;
  if (strcmp(name, "ul") == 0) return TagAction::LIST_CONTAINER;

  // Headings
  if (strcmp(name, "h1") == 0) return TagAction::HEADING;
  if (strcmp(name, "h2") == 0) return TagAction::HEADING;
  if (strcmp(name, "h3") == 0) return TagAction::HEADING;
  if (strcmp(name, "h4") == 0) return TagAction::HEADING;

  // Special
  if (strcmp(name, "abbr") == 0) return TagAction::ABBR;
  if (strcmp(name, "var") == 0) return TagAction::VAR;
  if (strcmp(name, "span") == 0) return TagAction::SPAN;
  if (strcmp(name, "a") == 0) return TagAction::SPAN;            // Registered; no actual anchors in dict
  if (strcmp(name, "_root") == 0) return TagAction::REGISTERED;  // Synthetic wrapper used by render()

  // Wikitext annotation tags: t:XX, tr:XX, lang:XX, gloss:XX, pos:XX, sc:XX, alt:XX, id:XX
  if (strncmp(name, "t:", 2) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "tr:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "lang:", 5) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "gloss:", 6) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "pos:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "sc:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "alt:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "id:", 3) == 0) return TagAction::WIKI_ANNOT;

  return TagAction::STRIP_KEEP;  // Unknown — strip tag, keep text
}

const char* DictHtmlRenderer::findAttr(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Constructor / destructor / reset
// ---------------------------------------------------------------------------

DictHtmlRenderer::DictHtmlRenderer() {
  spans.reserve(64);
  textBuf.reserve(512);
  tagStack.reserve(8);
  htmlCarry_.reserve(128);
  htmlInput_.reserve(640);
  normalizedHtml_.reserve(640);
}

DictHtmlRenderer::~DictHtmlRenderer() {
  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

void DictHtmlRenderer::reset() {
  spans.clear();
  textBuf.clear();
  pendingText.clear();
  htmlCarry_.clear();
  htmlInput_.clear();
  normalizedHtml_.clear();
  discardTagUntilClose_ = false;
  tagStack.clear();
  parseError = false;
  fmt = FormatState{};
  newlinePending = false;
  listItemPending = false;
  spanSink_ = SpanSink{};  // batch mode by default; streaming sets it after reset()

  // Reuse the existing parser (cheap reset) rather than free+create on every
  // render. Only the first call (null parser) allocates. XML_ParserReset clears
  // handlers + user data; callers re-apply them after reset (parseOpenFile,
  // render). Pays off because the renderer is now a reused activity member.
  if (parser) {
    XML_ParserReset(parser, nullptr);
  } else {
    parser = XML_ParserCreate(nullptr);
  }

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  unknownTagCount = 0;
#endif
}

// ---------------------------------------------------------------------------
// Text management
// ---------------------------------------------------------------------------

void DictHtmlRenderer::pushSpan() {
  if (pendingText.empty()) return;

  StyledSpan span;
  span.bold = fmt.bold;
  span.italic = fmt.italic;
  span.underline = fmt.underline;
  span.strikethrough = fmt.strikethrough;
  span.indentLevel = fmt.indentLevel;
  span.newlineBefore = newlinePending;
  span.isListItem = listItemPending;

  if (spanSink_.fn) {
    // Streaming: deliver the span immediately. text points into pendingText and
    // is valid only for the duration of the call — the sink copies it. textBuf
    // and the spans vector are never grown.
    span.text = pendingText.c_str();
    span.textOffset = 0;
    spanSink_(span);
  } else {
    // Batch: accumulate text into textBuf; span.text pointers fixed up after the
    // full parse (textBuf reallocates as it grows).
    span.textOffset = static_cast<uint32_t>(textBuf.size());
    if (textBuf.size() + pendingText.size() + 1 > textBuf.capacity()) {
      textBuf.reserve(textBuf.capacity() + 512);
    }
    textBuf.insert(textBuf.end(), pendingText.begin(), pendingText.end());
    textBuf.push_back('\0');
    spans.push_back(span);
  }

  pendingText.clear();
  newlinePending = false;
  listItemPending = false;
}

void DictHtmlRenderer::emitText(const char* s, int len) {
  if (len <= 0) return;
  for (int i = 0; i < len; i++) {
    if (i + 2 < len && static_cast<unsigned char>(s[i]) >= 0xE0 && static_cast<unsigned char>(s[i]) <= 0xEF &&
        (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
      const uint32_t cp = ((static_cast<uint32_t>(s[i]) & 0x0F) << 12) |
                          ((static_cast<uint32_t>(s[i + 1]) & 0x3F) << 6) | (static_cast<uint32_t>(s[i + 2]) & 0x3F);
      if (isDictionaryBullet(cp)) {
        pendingText += "\xE2\x80\xA2";  // U+2022 BULLET
        i += 2;
        continue;
      }
    }
    char c = s[i];
    if (c == '\r' || c == '\n') {
      pushSpan();
      newlinePending = true;
      // Treat \r\n as a single line ending
      if (c == '\r' && i + 1 < len && s[i + 1] == '\n') i++;
      continue;
    }
    if (c == '\t') c = ' ';
    pendingText += c;
  }
}

void DictHtmlRenderer::flushPending() { pushSpan(); }

// ---------------------------------------------------------------------------
// Expat callbacks
// ---------------------------------------------------------------------------

void XMLCALL DictHtmlRenderer::onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  TagAction action = classify(name);

  // Propagate suppression from parent
  bool parentSuppresses = !self->tagStack.empty() && self->tagStack.back().suppressChildren;

  StackEntry entry{};
  entry.action = action;
  entry.savedFmt = self->fmt;
  entry.suppressChildren = parentSuppresses;
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  entry.pendingLenAtOpen = static_cast<int>(self->pendingText.size());
  entry.unknownTagName[0] = '\0';
#endif

  if (!parentSuppresses) {
    switch (action) {
      case TagAction::BLOCK_STRIP:
        self->flushPending();
        entry.suppressChildren = true;
        break;

      case TagAction::FORMAT_BOLD:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::FORMAT_ITALIC:
      case TagAction::VAR:
        self->flushPending();
        self->fmt.italic = true;
        break;

      case TagAction::FORMAT_UNDERLINE:
        self->flushPending();
        self->fmt.underline = true;
        break;

      case TagAction::FORMAT_STRIKE:
        self->flushPending();
        self->fmt.strikethrough = true;
        break;

      case TagAction::FORMAT_SMALL:
        self->flushPending();
        break;

      case TagAction::FORMAT_CODE:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        self->newlinePending = true;
        self->listItemPending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.bold = true;
        break;

      case TagAction::ABBR: {
        self->flushPending();  // Flush preceding text before body accumulates
        const char* title = findAttr(atts, "title");
        if (title) {
          entry.abbrTitle = title;
        }
        break;
      }

      case TagAction::WIKI_ANNOT: {
        const char* colon = strchr(name, ':');
        if (colon && colon[1] != '\0') {
          entry.wikiText = colon + 1;
        }
        break;
      }

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        strncpy(entry.unknownTagName, name, sizeof(entry.unknownTagName) - 1);
        entry.unknownTagName[sizeof(entry.unknownTagName) - 1] = '\0';
        entry.pendingLenAtOpen = static_cast<int>(self->pendingText.size());
#endif
        break;

      default:
        break;
    }
  }

  self->tagStack.push_back(entry);
}

void XMLCALL DictHtmlRenderer::onEnd(void* ud, const XML_Char* name) {
  (void)name;
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;
  if (self->tagStack.empty()) return;

  StackEntry& entry = self->tagStack.back();
  bool suppressed = entry.suppressChildren || entry.action == TagAction::BLOCK_STRIP;

  if (!suppressed) {
    switch (entry.action) {
      case TagAction::FORMAT_BOLD:
      case TagAction::FORMAT_ITALIC:
      case TagAction::FORMAT_UNDERLINE:
      case TagAction::FORMAT_STRIKE:
      case TagAction::FORMAT_SMALL:
      case TagAction::FORMAT_CODE:
      case TagAction::VAR:
      case TagAction::SPAN:
      case TagAction::REGISTERED:
        self->flushPending();
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::ABBR:
        if (!entry.abbrTitle.empty()) {
          self->flushPending();
          std::string expanded = " (" + entry.abbrTitle + ")";
          self->emitText(expanded.c_str(), static_cast<int>(expanded.size()));
          self->flushPending();
        }
        break;

      case TagAction::WIKI_ANNOT:
        if (!entry.wikiText.empty()) {
          self->flushPending();  // Flush preceding text before emitting wiki suffix
          self->emitText(entry.wikiText.c_str(), static_cast<int>(entry.wikiText.size()));
          self->flushPending();
        }
        break;

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        if (entry.unknownTagName[0] != '\0') {
          // Extract word before (from text accumulated before tag opened)
          char wordBefore[32] = {};
          // We can't easily recover the text before — use empty for now
          // Extract tag contents: text accumulated between open and close
          char tagContents[64] = {};
          int contStart = entry.pendingLenAtOpen;
          int contLen = static_cast<int>(self->pendingText.size()) - contStart;
          if (contLen > 0 && contLen < static_cast<int>(sizeof(tagContents))) {
            memcpy(tagContents, self->pendingText.c_str() + contStart, contLen);
            tagContents[contLen] = '\0';
          }
          self->recordUnknownTag(entry.unknownTagName, wordBefore, tagContents, "");
        }
#endif
        self->flushPending();
        break;

      default:
        break;
    }

    // Restore saved format (bold/italic/underline/strikethrough/indentLevel)
    self->fmt = entry.savedFmt;
  }

  self->tagStack.pop_back();
}

void XMLCALL DictHtmlRenderer::onText(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  if (!self->tagStack.empty()) {
    const StackEntry& top = self->tagStack.back();
    // Suppress text inside block-strip and for wiki-annot (emit from tag name, not body)
    if (top.suppressChildren || top.action == TagAction::WIKI_ANNOT || top.action == TagAction::BLOCK_STRIP) {
      return;
    }
  }

  self->emitText(s, len);
}

// ---------------------------------------------------------------------------
// HTML entity resolver — feeds definition to expat incrementally
// ---------------------------------------------------------------------------

// Returns the UTF-8 replacement for a known HTML named entity (matched by
// in-place strncmp — no copy, no stack buffer). Returns nullptr for unknowns.
// Empty string ("") means the entity should be silently dropped (zero-width chars).
static const char* lookupHtmlEntity(const char* name, int nameLen) {
  struct E {
    const char* n;
    int nLen;
    const char* utf8;
  };
  static constexpr E kTable[] = {
      {"lsqb", 4, "["},
      {"rsqb", 4, "]"},
      {"nbsp", 4, " "},
      {"ndash", 5, "\xE2\x80\x93"},   // –
      {"mdash", 5, "\xE2\x80\x94"},   // —
      {"hellip", 6, "\xE2\x80\xA6"},  // …
      {"bull", 4, "\xE2\x80\xA2"},    // •
      {"dagger", 6, "\xE2\x80\xA0"},  // †
      {"sect", 4, "\xC2\xA7"},        // §
      {"para", 4, "\xC2\xB6"},        // ¶
      {"prime", 5, "\xE2\x80\xB2"},   // ′
      {"Prime", 5, "\xE2\x80\xB3"},   // ″
      {"times", 5, "\xC3\x97"},       // ×
      {"minus", 5, "\xE2\x88\x92"},   // −
      {"middot", 6, "\xC2\xB7"},      // ·
      {"lrm", 3, ""},                 // LEFT-TO-RIGHT MARK — zero-width, drop
      {"rlm", 3, ""},                 // RIGHT-TO-LEFT MARK — zero-width, drop
  };
  for (const auto& e : kTable) {
    if (e.nLen == nameLen && strncmp(name, e.n, nameLen) == 0) return e.utf8;
  }
  return nullptr;
}

void DictHtmlRenderer::normalizeHtmlChunk(const char* buf, int len, bool isLast) {
  static constexpr size_t kMaxCarriedTag = 512;

  htmlInput_.assign(htmlCarry_);
  htmlInput_.append(buf, static_cast<size_t>(len));
  htmlCarry_.clear();
  normalizedHtml_.clear();

  size_t i = 0;
  if (discardTagUntilClose_) {
    const size_t close = htmlInput_.find('>');
    if (close == std::string::npos) return;
    discardTagUntilClose_ = false;
    i = close + 1;
  }

  while (i < htmlInput_.size()) {
    if (htmlInput_[i] != '<') {
      const unsigned char c = static_cast<unsigned char>(htmlInput_[i++]);
      // XML rejects these bytes even though they occur in some legacy HTML
      // dictionaries. Outside discarded resource tags, a space is the least
      // surprising readable fallback.
      normalizedHtml_ += (c < 0x20 && c != '\t' && c != '\r' && c != '\n') ? ' ' : static_cast<char>(c);
      continue;
    }

    char quote = '\0';
    size_t close = i + 1;
    for (; close < htmlInput_.size(); close++) {
      const char c = htmlInput_[close];
      if (quote != '\0') {
        if (c == quote) quote = '\0';
      } else if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }

    if (close == htmlInput_.size()) {
      if (isLast) {
        // Preserve malformed trailing markup as visible text without giving
        // Expat a bare '<'.
        normalizedHtml_ += "&lt;";
        normalizedHtml_.append(htmlInput_, i + 1, std::string::npos);
      } else if (htmlInput_.size() - i <= kMaxCarriedTag) {
        htmlCarry_.assign(htmlInput_, i, std::string::npos);
      } else {
        // A hostile or corrupt tag must not grow scratch to definition size.
        discardTagUntilClose_ = true;
      }
      return;
    }

    size_t nameStart = i + 1;
    while (nameStart < close && (htmlInput_[nameStart] == ' ' || htmlInput_[nameStart] == '\t')) nameStart++;
    const bool closing = nameStart < close && htmlInput_[nameStart] == '/';
    if (closing) nameStart++;
    const size_t nameBegin = nameStart;
    while (nameStart < close) {
      const char c = htmlInput_[nameStart];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') break;
      nameStart++;
    }
    const int nameLen = static_cast<int>(nameStart - nameBegin);

    if (!closing && (rangeTagEquals(htmlInput_.data() + nameBegin, nameLen, "br") ||
                     rangeTagEquals(htmlInput_.data() + nameBegin, nameLen, "hr"))) {
      // Canonical XML form. HR is represented as a line break because the
      // e-ink definition view has no horizontal-rule primitive.
      normalizedHtml_ += "<br/>";
    } else if (isDiscardedVoidTag(htmlInput_.data() + nameBegin, nameLen)) {
      // Omit the whole element, including binary-ish resource attributes. Its
      // surrounding text remains in the stream.
    } else {
      normalizedHtml_.append(htmlInput_, i, close - i + 1);
    }
    i = close + 1;
  }
}

// ---------------------------------------------------------------------------
// Public render()
// ---------------------------------------------------------------------------

const std::vector<StyledSpan>& DictHtmlRenderer::render(const char* html, int len) {
  reset();

  if (!parser) {
    parseError = true;
    return spans;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  // Wrap fragment in a synthetic root element so expat sees well-formed XML
  static constexpr const char kOpen[] = "<_root>";
  static constexpr const char kClose[] = "</_root>";

  if (XML_Parse(parser, kOpen, static_cast<int>(sizeof(kOpen) - 1), 0) != XML_STATUS_OK) {
    parseError = true;
  }

  normalizeHtmlChunk(html, len, true);
  html = normalizedHtml_.data();
  len = static_cast<int>(normalizedHtml_.size());

  // Walk the definition, feeding normal text runs directly to expat and
  // resolving HTML named entities inline. No heap allocation; entity names
  // compared in-place with strncmp — no copy, no extra stack buffer.
  if (!parseError) {
    static const char* const kXmlBuiltins[] = {"amp", "lt", "gt", "quot", "apos"};
    int i = 0;
    while (i < len && !parseError) {
      // Feed run of non-entity characters directly (zero copy)
      int runStart = i;
      while (i < len && html[i] != '&') i++;
      if (i > runStart) {
        if (XML_Parse(parser, html + runStart, i - runStart, 0) != XML_STATUS_OK) {
          parseError = true;
          break;
        }
      }
      if (i >= len) break;

      // html[i] == '&' — scan for matching ';'
      int j = i + 1;
      while (j < len && html[j] != ';' && html[j] != '&' && html[j] != '<' && (j - i) < 33) j++;

      if (j < len && html[j] == ';') {
        int nameLen = j - i - 1;
        const char* nameStart = html + i + 1;

        // Numeric references (&#NN; / &#xNN;) and XML built-ins pass through unchanged
        bool passThrough = (nameLen > 0 && nameStart[0] == '#');
        if (!passThrough) {
          for (const auto* b : kXmlBuiltins) {
            if (static_cast<int>(strlen(b)) == nameLen && strncmp(nameStart, b, nameLen) == 0) {
              passThrough = true;
              break;
            }
          }
        }

        if (passThrough) {
          if (XML_Parse(parser, html + i, j - i + 1, 0) != XML_STATUS_OK) {
            parseError = true;
            break;
          }
        } else {
          const char* repl = lookupHtmlEntity(nameStart, nameLen);
          if (repl && repl[0] != '\0') {
            if (XML_Parse(parser, repl, static_cast<int>(strlen(repl)), 0) != XML_STATUS_OK) {
              parseError = true;
              break;
            }
          } else if (!repl) {
            // Preserve unsupported named entities visibly instead of deleting
            // them. Escaping the ampersand keeps the XML stream valid.
            static constexpr char kAmp[] = "&amp;";
            if (XML_Parse(parser, kAmp, static_cast<int>(sizeof(kAmp) - 1), 0) != XML_STATUS_OK ||
                XML_Parse(parser, nameStart, nameLen, 0) != XML_STATUS_OK ||
                XML_Parse(parser, ";", 1, 0) != XML_STATUS_OK) {
              parseError = true;
              break;
            }
          }
        }
        i = j + 1;
      } else {
        // Preserve a malformed ampersand as text without invalidating XML.
        static constexpr char kAmp[] = "&amp;";
        if (XML_Parse(parser, kAmp, static_cast<int>(sizeof(kAmp) - 1), 0) != XML_STATUS_OK) {
          parseError = true;
          break;
        }
        i++;
      }
    }
  }

  if (!parseError) {
    if (XML_Parse(parser, kClose, static_cast<int>(sizeof(kClose) - 1), 1) != XML_STATUS_OK) {
      parseError = true;
    }
  }

  flushPending();

  // textBuf is now stable — convert offsets to pointers.
  // Done even on parse error so that partially accumulated spans are usable.
  for (auto& s : spans) {
    s.text = textBuf.data() + s.textOffset;
  }

  return spans;
}

// ---------------------------------------------------------------------------
// Chunked entity resolution (shared by render() and renderFromFile())
// ---------------------------------------------------------------------------

void DictHtmlRenderer::feedEntityResolved(const char* buf, int len, bool isLast, char* carry, int* carryLen) {
  static const char* const kXmlBuiltins[] = {"amp", "lt", "gt", "quot", "apos"};
  int i = 0;
  while (i < len && !parseError) {
    int runStart = i;
    while (i < len && buf[i] != '&') i++;
    if (i > runStart) {
      if (XML_Parse(parser, buf + runStart, i - runStart, 0) != XML_STATUS_OK) {
        parseError = true;
        return;
      }
    }
    if (i >= len) break;

    // buf[i] == '&' — scan for ';'
    int j = i + 1;
    while (j < len && buf[j] != ';' && buf[j] != '&' && buf[j] != '<' && (j - i) < 33) j++;

    if (j >= len && !isLast) {
      // Entity spans chunk boundary — carry remainder for next chunk
      if (carry && carryLen) {
        int remaining = len - i;
        if (remaining > 63) remaining = 63;  // safety cap
        memcpy(carry, buf + i, remaining);
        *carryLen = remaining;
      }
      return;
    }

    if (j < len && buf[j] == ';') {
      int nameLen = j - i - 1;
      const char* nameStart = buf + i + 1;

      bool passThrough = (nameLen > 0 && nameStart[0] == '#');
      if (!passThrough) {
        for (const auto* b : kXmlBuiltins) {
          if (static_cast<int>(strlen(b)) == nameLen && strncmp(nameStart, b, nameLen) == 0) {
            passThrough = true;
            break;
          }
        }
      }

      if (passThrough) {
        if (XML_Parse(parser, buf + i, j - i + 1, 0) != XML_STATUS_OK) {
          parseError = true;
          return;
        }
      } else {
        const char* repl = lookupHtmlEntity(nameStart, nameLen);
        if (repl && repl[0] != '\0') {
          if (XML_Parse(parser, repl, static_cast<int>(strlen(repl)), 0) != XML_STATUS_OK) {
            parseError = true;
            return;
          }
        } else if (!repl) {
          static constexpr char kAmp[] = "&amp;";
          if (XML_Parse(parser, kAmp, static_cast<int>(sizeof(kAmp) - 1), 0) != XML_STATUS_OK ||
              XML_Parse(parser, nameStart, nameLen, 0) != XML_STATUS_OK ||
              XML_Parse(parser, ";", 1, 0) != XML_STATUS_OK) {
            parseError = true;
            return;
          }
        }
      }
      i = j + 1;
    } else {
      // Preserve a malformed ampersand as text without invalidating XML.
      static constexpr char kAmp[] = "&amp;";
      if (XML_Parse(parser, kAmp, static_cast<int>(sizeof(kAmp) - 1), 0) != XML_STATUS_OK) {
        parseError = true;
        return;
      }
      i++;
    }
  }
}

void DictHtmlRenderer::feedPlainText(const char* buf, int len, bool isLast, char* carry, int* carryLen) {
  int i = 0;
  while (i < len) {
    if (buf[i] == '<') {
      const char* close = static_cast<const char*>(memchr(buf + i + 1, '>', static_cast<size_t>(len - i - 1)));
      if (!close) {
        // normalizeHtmlChunk() carries incomplete tags, so this is only a
        // defensive fallback for corrupt input.
        emitText("<", 1);
        i++;
        continue;
      }

      int nameStart = i + 1;
      while (nameStart < close - buf && (buf[nameStart] == ' ' || buf[nameStart] == '\t')) nameStart++;
      if (nameStart < close - buf && buf[nameStart] == '/') nameStart++;
      const int nameBegin = nameStart;
      while (nameStart < close - buf) {
        const char c = buf[nameStart];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') break;
        nameStart++;
      }

      const int nameLen = nameStart - nameBegin;
      const bool isBreak =
          rangeTagEquals(buf + nameBegin, nameLen, "br") || rangeTagEquals(buf + nameBegin, nameLen, "hr") ||
          rangeTagEquals(buf + nameBegin, nameLen, "p") || rangeTagEquals(buf + nameBegin, nameLen, "div") ||
          rangeTagEquals(buf + nameBegin, nameLen, "ol") || rangeTagEquals(buf + nameBegin, nameLen, "ul") ||
          rangeTagEquals(buf + nameBegin, nameLen, "li") ||
          (nameLen == 2 && asciiLower(buf[nameBegin]) == 'h' && buf[nameBegin + 1] >= '1' && buf[nameBegin + 1] <= '6');
      if (isBreak) {
        flushPending();
        newlinePending = true;
      }
      i = static_cast<int>(close - buf) + 1;
      continue;
    }

    if (buf[i] != '&') {
      const int runStart = i;
      while (i < len && buf[i] != '<' && buf[i] != '&') i++;
      emitText(buf + runStart, i - runStart);
      continue;
    }

    int end = i + 1;
    while (end < len && buf[end] != ';' && buf[end] != '&' && buf[end] != '<' && end - i < 33) end++;
    if (end >= len && !isLast) {
      if (carry && carryLen) {
        int remaining = len - i;
        if (remaining > 63) remaining = 63;
        memcpy(carry, buf + i, static_cast<size_t>(remaining));
        *carryLen = remaining;
      }
      return;
    }

    if (end >= len || buf[end] != ';') {
      emitText("&", 1);
      i++;
      continue;
    }

    const char* name = buf + i + 1;
    const int nameLen = end - i - 1;
    const char* replacement = nullptr;
    if (nameLen == 3 && strncmp(name, "amp", 3) == 0) replacement = "&";
    if (nameLen == 2 && strncmp(name, "lt", 2) == 0) replacement = "<";
    if (nameLen == 2 && strncmp(name, "gt", 2) == 0) replacement = ">";
    if (nameLen == 4 && strncmp(name, "quot", 4) == 0) replacement = "\"";
    if (nameLen == 4 && strncmp(name, "apos", 4) == 0) replacement = "'";
    if (!replacement) replacement = lookupHtmlEntity(name, nameLen);

    if (replacement) {
      emitText(replacement, static_cast<int>(strlen(replacement)));
    } else if (nameLen > 1 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      uint32_t cp = 0;
      bool valid = nameLen > (hex ? 2 : 1);
      for (int digitIndex = hex ? 2 : 1; digitIndex < nameLen; digitIndex++) {
        const char c = name[digitIndex];
        uint8_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<uint8_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
          digit = static_cast<uint8_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
          digit = static_cast<uint8_t>(c - 'A' + 10);
        } else {
          valid = false;
          break;
        }
        cp = cp * (hex ? 16u : 10u) + digit;
        if (cp > 0x10FFFF) {
          valid = false;
          break;
        }
      }
      char utf8[4];
      const int utf8Len = valid && cp != 0 ? encodeUtf8(cp, utf8) : 0;
      if (utf8Len > 0) {
        emitText(utf8, utf8Len);
      } else {
        emitText(buf + i, end - i + 1);
      }
    } else {
      // Unknown entities are more useful visible than silently deleted.
      emitText(buf + i, end - i + 1);
    }
    i = end + 1;
  }
}

// ---------------------------------------------------------------------------
// Stream-render from .dict file (definition never held in RAM)
// ---------------------------------------------------------------------------

void DictHtmlRenderer::parseOpenFile(HalFile& file, uint32_t size) {
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  static constexpr const char kOpen[] = "<_root>";
  static constexpr const char kClose[] = "</_root>";

  if (XML_Parse(parser, kOpen, static_cast<int>(sizeof(kOpen) - 1), 0) != XML_STATUS_OK) {
    parseError = true;
  }

  if (!parseError) {
    char chunk[512];
    char entityCarry[64];
    int entityCarryLen = 0;
    uint32_t remaining = size;

    while (remaining > 0 && !parseError) {
      const uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
      int n = file.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
      if (n <= 0) break;
      remaining -= static_cast<uint32_t>(n);
      normalizeHtmlChunk(chunk, n, remaining == 0);

      if (entityCarryLen > 0) normalizedHtml_.insert(0, entityCarry, static_cast<size_t>(entityCarryLen));
      entityCarryLen = 0;
      feedEntityResolved(normalizedHtml_.data(), static_cast<int>(normalizedHtml_.size()),
                         remaining == 0 && htmlCarry_.empty() && !discardTagUntilClose_, entityCarry, &entityCarryLen);
    }

    // Flush any trailing carryover (e.g. malformed entity at end of file)
    if (entityCarryLen > 0 && !parseError) {
      feedEntityResolved(entityCarry, entityCarryLen, true, nullptr, nullptr);
    }
  }

  if (!parseError) {
    if (XML_Parse(parser, kClose, static_cast<int>(sizeof(kClose) - 1), 1) != XML_STATUS_OK) {
      parseError = true;
    }
  }

  flushPending();

  if (parseError) {
    LOG_ERR("DHTML", "Parse failed at byte %ld: %s", static_cast<long>(XML_GetCurrentByteIndex(parser)),
            XML_ErrorString(XML_GetErrorCode(parser)));
  }
}

const std::vector<StyledSpan>& DictHtmlRenderer::renderFromFile(const char* dictPath, uint32_t offset, uint32_t size) {
  reset();

  if (!parser) {
    parseError = true;
    return spans;
  }

  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath, file)) {
    LOG_ERR("DHTML", "Failed to open: %s", dictPath);
    parseError = true;
    return spans;
  }
  file.seekSet(offset);

  parseOpenFile(file, size);
  file.close();

  for (auto& s : spans) {
    s.text = textBuf.data() + s.textOffset;
  }

  return spans;
}

bool DictHtmlRenderer::renderFromFileStreaming(const char* dictPath, uint32_t offset, uint32_t size,
                                               const SpanSink& sink) {
  reset();
  spanSink_ = sink;  // install for this parse; pushSpan() now streams to the sink

  if (!parser) {
    parseError = true;
    spanSink_ = SpanSink{};
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath, file)) {
    LOG_ERR("DHTML", "Failed to open: %s", dictPath);
    parseError = true;
    spanSink_ = SpanSink{};
    return false;
  }
  file.seekSet(offset);

  parseOpenFile(file, size);
  file.close();

  spanSink_ = SpanSink{};  // streaming never materializes spans/textBuf — nothing to fix up
  return !parseError;
}

bool DictHtmlRenderer::renderPlainTextFromFileStreaming(const char* dictPath, uint32_t offset, uint32_t size,
                                                        const SpanSink& sink) {
  reset();
  spanSink_ = sink;

  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath, file)) {
    LOG_ERR("DHTML", "Failed to open plain-text fallback: %s", dictPath);
    spanSink_ = SpanSink{};
    return false;
  }
  if (!file.seekSet(offset)) {
    LOG_ERR("DHTML", "Failed to seek plain-text fallback: %s", dictPath);
    file.close();
    spanSink_ = SpanSink{};
    return false;
  }

  // Matches the normal streaming path: one SD read buffer plus a bounded
  // cross-chunk entity carry. No definition-sized fallback allocation.
  char chunk[512];
  char entityCarry[64];
  int entityCarryLen = 0;
  uint32_t remaining = size;
  bool readOk = true;
  while (remaining > 0) {
    const uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
    const int n = file.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
    if (n <= 0) {
      readOk = false;
      break;
    }
    remaining -= static_cast<uint32_t>(n);
    normalizeHtmlChunk(chunk, n, remaining == 0);
    if (entityCarryLen > 0) normalizedHtml_.insert(0, entityCarry, static_cast<size_t>(entityCarryLen));
    entityCarryLen = 0;
    feedPlainText(normalizedHtml_.data(), static_cast<int>(normalizedHtml_.size()),
                  remaining == 0 && htmlCarry_.empty() && !discardTagUntilClose_, entityCarry, &entityCarryLen);
  }
  if (entityCarryLen > 0 && readOk) feedPlainText(entityCarry, entityCarryLen, true, nullptr, nullptr);
  flushPending();
  file.close();
  spanSink_ = SpanSink{};

  if (!readOk) LOG_ERR("DHTML", "Failed reading plain-text fallback: %s", dictPath);
  return readOk;
}

// ---------------------------------------------------------------------------
// Unknown tag tracking (smoke test only)
// ---------------------------------------------------------------------------

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN

void DictHtmlRenderer::recordUnknownTag(const char* tagName, const char* wordBefore, const char* tagContents,
                                        const char* wordAfter) {
  if (unknownTagCount >= MAX_UNKNOWN_TAGS) return;
  UnknownTagInfo& info = unknownTags[unknownTagCount++];
  strncpy(info.tag, tagName, sizeof(info.tag) - 1);
  info.tag[sizeof(info.tag) - 1] = '\0';
  strncpy(info.entry, currentEntryName ? currentEntryName : "", sizeof(info.entry) - 1);
  info.entry[sizeof(info.entry) - 1] = '\0';
  strncpy(info.wordBefore, wordBefore ? wordBefore : "", sizeof(info.wordBefore) - 1);
  info.wordBefore[sizeof(info.wordBefore) - 1] = '\0';
  strncpy(info.tagContents, tagContents ? tagContents : "", sizeof(info.tagContents) - 1);
  info.tagContents[sizeof(info.tagContents) - 1] = '\0';
  strncpy(info.wordAfter, wordAfter ? wordAfter : "", sizeof(info.wordAfter) - 1);
  info.wordAfter[sizeof(info.wordAfter) - 1] = '\0';
}

#endif  // DICT_HTML_RENDERER_TRACK_UNKNOWN
