#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Maximum number of two-part descendant rules (ancestor subject) to store
constexpr size_t MAX_DESCENDANT_RULES = CssParser::MAX_DESCENDANT_RULES;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;
constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
constexpr size_t CSS_FIXED_STYLE_BYTES = 4 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) +
                                         4 * sizeof(uint8_t) + 2 * sizeof(uint8_t) + sizeof(uint32_t);
static_assert(CSS_FIXED_STYLE_BYTES == 69,
              "CssStyle cache payload changed; update read/writeCssStylePayload and bump CSS_CACHE_VERSION");

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

// Case-insensitive ASCII substring search. Only needed by text-decoration,
// which accepts multi-value strings like "underline solid red".
constexpr bool icontainsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::search(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                     [](char a, char b) { return asciiToLower(a) == b; }) != value.end();
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

// FNV-1a per Fowler/Noll/Vo, sized to match size_t on the target. The firmware
// runs on a 32-bit core where size_t is 32 bits, so naively using the 64-bit
// constants would silently truncate FNV_PRIME to a non-prime and wreck hash
// distribution. The selection below picks the canonical 32- or 64-bit
// constants at compile time so the same source works in a 64-bit host
// simulator. `fnv1aMix` is the per-byte mix step; callers apply any
// byte-level transform (e.g. asciiToLower) first.
static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV constants are only defined for 32- or 64-bit size_t");
constexpr size_t FNV_OFFSET_BASIS =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME =
    sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

constexpr size_t fnv1aMix(size_t hash, unsigned char byte) { return (hash ^ byte) * FNV_PRIME; }

// Parse the entirety of s as a number into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool tryInterpretCssPageBreak(std::string_view value, bool& out) {
  value = trimCssWhitespace(stripTrailingImportant(value));
  if (iequalsAscii(value, "always") || iequalsAscii(value, "page") || iequalsAscii(value, "left") ||
      iequalsAscii(value, "right")) {
    out = true;
    return true;
  }
  if (iequalsAscii(value, "auto") || iequalsAscii(value, "avoid") || iequalsAscii(value, "avoid-page")) {
    out = false;
    return true;
  }
  return false;
}

bool tryInterpretBackgroundBlack(std::string_view value, bool& out) {
  value = stripTrailingImportant(value);
  value = trimCssWhitespace(value);

  if (value.empty()) {
    return false;
  }

  bool sawExplicitNonBlack = false;
  size_t tokenStart = 0;
  for (size_t i = 0; i <= value.size(); ++i) {
    if (i == value.size() || isCssWhitespace(value[i])) {
      if (i > tokenStart) {
        const std::string_view token = value.substr(tokenStart, i - tokenStart);
        if (iequalsAscii(token, "black") || token == "#000" || token == "#000000") {
          out = true;
          return true;
        }
        if (iequalsAscii(token, "white") || token == "#fff" || token == "#ffffff" ||
            iequalsAscii(token, "transparent") || iequalsAscii(token, "none")) {
          sawExplicitNonBlack = true;
        }
      }
      tokenStart = i + 1;
    }
  }

  std::string compact;
  compact.reserve(value.size());
  for (const char c : value) {
    if (!isCssWhitespace(c)) {
      compact.push_back(asciiToLower(c));
    }
  }

  if (compact == "rgb(0,0,0)" || compact == "rgba(0,0,0,1)" || compact == "rgba(0,0,0,1.0)" ||
      compact.find("rgb(0,0,0)") != std::string::npos || compact.find("rgba(0,0,0,1)") != std::string::npos ||
      compact.find("rgba(0,0,0,1.0)") != std::string::npos) {
    out = true;
    return true;
  }

  if (sawExplicitNonBlack || compact == "transparent" || compact == "none") {
    out = false;
    return true;
  }

  return false;
}

}  // anonymous namespace

// Transparent case-insensitive hash/equal. Bodies live here (rather than
// inline in the header) so they can share the anonymous-namespace asciiToLower
// with the other ASCII helpers in this translation unit.

size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept {
  size_t h = FNV_OFFSET_BASIS;
  for (char c : sv) h = fnv1aMix(h, asciiToLower(c));
  return h;
}

size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

size_t CssParser::SvHash::operator()(CompositeKey k) const noexcept {
  // Hash the case-folded concatenation of every piece without materializing
  // it — the running hash continues across pieces as if they were one buffer.
  size_t h = FNV_OFFSET_BASIS;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) h = fnv1aMix(h, asciiToLower(c));
  }
  return h;
}

bool CssParser::SvEqual::operator()(std::string_view a, std::string_view b) const noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
  }
  return true;
}

bool CssParser::SvEqual::operator()(const std::string& a, std::string_view b) const noexcept {
  return operator()(std::string_view(a), b);
}

bool CssParser::SvEqual::operator()(std::string_view a, const std::string& b) const noexcept {
  return operator()(a, std::string_view(b));
}

bool CssParser::SvEqual::operator()(const std::string& a, const std::string& b) const noexcept {
  return operator()(std::string_view(a), std::string_view(b));
}

bool CssParser::SvEqual::operator()(CompositeKey k, std::string_view sv) const noexcept {
  size_t total = 0;
  for (std::string_view piece : k.pieces) total += piece.size();
  if (total != sv.size()) return false;
  size_t i = 0;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) {
      if (asciiToLower(c) != asciiToLower(sv[i++])) return false;
    }
  }
  return true;
}

bool CssParser::SvEqual::operator()(std::string_view sv, CompositeKey k) const noexcept { return operator()(k, sv); }

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values; check most specific first.
  if (icontainsAscii(val, "line-through")) {
    return CssTextDecoration::LineThrough;
  }
  if (icontainsAscii(val, "underline")) {
    return CssTextDecoration::Underline;
  }
  return CssTextDecoration::None;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseNumber(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  const std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    const std::string_view displayValue = stripTrailingImportant(value);
    style.display = iequalsAscii(displayValue, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "background") || iequalsAscii(name, "background-color")) {
    bool backgroundBlack = false;
    if (tryInterpretBackgroundBlack(value, backgroundBlack)) {
      style.backgroundBlack = backgroundBlack;
      style.defined.backgroundBlack = 1;
    }
  } else if (iequalsAscii(name, "direction")) {
    const std::string_view directionValue = stripTrailingImportant(value);
    if (iequalsAscii(directionValue, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(directionValue, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  } else if (iequalsAscii(name, "page-break-before") || iequalsAscii(name, "break-before")) {
    bool pageBreakBefore = false;
    if (tryInterpretCssPageBreak(value, pageBreakBefore)) {
      style.pageBreakBefore = pageBreakBefore;
      style.defined.pageBreakBefore = 1;
    }
  } else if (iequalsAscii(name, "page-break-after") || iequalsAscii(name, "break-after")) {
    bool pageBreakAfter = false;
    if (tryInterpretCssPageBreak(value, pageBreakAfter)) {
      style.pageBreakAfter = pageBreakAfter;
      style.defined.pageBreakAfter = 1;
    }
  }
}

CssStyle CssParser::parseDeclarations(std::string_view declBlock) {
  CssStyle style;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        parseDeclarationIntoStyle(declBlock.substr(start, i - start), style);
      }
      start = i + 1;
    }
  }

  return style;
}

// Returns true if a simple selector (tag, .class, or tag.class) matches the element.
// Matching is ASCII case-insensitive; class tokens are read without allocation.
bool CssParser::selectorMatchesElement(std::string_view selector, std::string_view tag, std::string_view classAttr) {
  if (selector.empty()) return false;

  const size_t dotPos = selector.find('.');
  if (dotPos == std::string::npos) {
    return iequalsAscii(selector, tag);
  }

  const std::string_view selectorTag(selector.data(), dotPos);
  const std::string_view selectorClass(selector.data() + dotPos + 1, selector.size() - dotPos - 1);

  if (!selectorTag.empty() && !iequalsAscii(selectorTag, tag)) return false;

  if (classAttr.empty()) return false;
  bool matched = false;
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (iequalsAscii(cls, selectorClass)) matched = true;
  });
  return matched;
}

// Rule processing

bool CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_ERR("CSS", "Reached max rules limit (%zu), treating CSS parse as incomplete", MAX_RULES);
    return false;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently; the only heap allocation per kept selector is the std::string
  // map key, which is unavoidable since the map owns its keys.
  bool limitReached = false;
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        constexpr std::string_view kUnsupportedSelectorChars = "+>[:#~*";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;

        const bool isDescendantSelector = sel.find_first_of(" \t\n\r\f") != std::string_view::npos;
        if (isDescendantSelector) {
          if (descendantRules_.size() >= MAX_DESCENDANT_RULES) return;

          std::string_view parts[2];
          size_t partCount = 0;
          forEachDelimitedToken(sel, isCssWhitespace, [&](std::string_view part) {
            if (partCount < 2) parts[partCount] = part;
            ++partCount;
          });
          if (partCount != 2) return;

          auto isSimpleSelector = [](std::string_view s) -> bool {
            int dotCount = 0;
            for (const char c : s) {
              if (c == '#' || c == ':' || c == '[' || c == '+' || c == '~' || c == '>' || c == '*') return false;
              if (c == '.') ++dotCount;
            }
            return dotCount <= 1;
          };
          if (!isSimpleSelector(parts[0]) || !isSimpleSelector(parts[1])) return;

          auto it = std::find_if(descendantRules_.begin(), descendantRules_.end(), [&](const DescendantRule& rule) {
            return iequalsAscii(rule.ancestorSelector, parts[0]) && iequalsAscii(rule.subjectSelector, parts[1]);
          });
          if (it != descendantRules_.end()) {
            it->style.applyOver(style);
          } else {
            descendantRules_.push_back({std::string(parts[0]), std::string(parts[1]), style});
          }
          return;
        }

        // Skip if this would exceed the rule limit
        if (rulesBySelector_.size() >= MAX_RULES) {
          LOG_ERR("CSS", "Reached max rules limit, treating CSS parse as incomplete");
          limitReached = true;
          return;
        }

        // Store or merge with existing. Hash/equal are case-insensitive, so two
        // selectors that differ only in ASCII case collide on insert and merge.
        auto it = rulesBySelector_.find(sel);
        if (it != rulesBySelector_.end()) {
          it->second.applyOver(style);
        } else {
          rulesBySelector_.emplace(std::string(sel), style);
        }
      });
  return !limitReached;
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  bool stopParsing = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
        }
        if (!skippingRule) {
          stopParsing = !processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (!stopParsing && source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead && !stopParsing; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (!stopParsing && maybeSlash) {
    handleChar('/');
  }

  if (stopParsing) {
    LOG_ERR("CSS", "CSS parse stopped after %zu bytes with %zu selector rules and %zu descendant rules loaded",
            totalRead, rulesBySelector_.size(), descendantRules_.size());
    return false;
  }

  LOG_DBG("CSS", "Parsed %zu rules and %zu descendant rules from %zu bytes", rulesBySelector_.size(),
          descendantRules_.size(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr,
                                 const std::vector<CssAncestorEntry>& ancestors) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getFreeHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  CssStyle result;

  // 1. Apply element-level style (lowest priority).
  CssStyle matchedStyle;
  if (lookupRule(tagName, matchedStyle)) {
    result.applyOver(matchedStyle);
  }

  // 2. Apply two-part descendant rules — higher specificity than bare element, lower than class.
  if (!ancestors.empty() && !descendantRules_.empty()) {
    for (const auto& rule : descendantRules_) {
      if (!selectorMatchesElement(rule.subjectSelector, tagName, classAttr)) continue;
      for (const auto& anc : ancestors) {
        if (selectorMatchesElement(rule.ancestorSelector, anc.tag, anc.classAttr)) {
          result.applyOver(rule.style);
          break;
        }
      }
    }
  }

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (cls.size() + 1 > MAX_SELECTOR_LENGTH) return;
    std::array<char, MAX_SELECTOR_LENGTH> selector{};
    selector[0] = '.';
    memcpy(selector.data() + 1, cls.data(), cls.size());
    if (lookupRule(std::string_view(selector.data(), cls.size() + 1), matchedStyle)) {
      result.applyOver(matchedStyle);
    }
  });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (tagName.size() + 1 + cls.size() > MAX_SELECTOR_LENGTH) return;
    std::array<char, MAX_SELECTOR_LENGTH> selector{};
    memcpy(selector.data(), tagName.data(), tagName.size());
    selector[tagName.size()] = '.';
    memcpy(selector.data() + tagName.size() + 1, cls.data(), cls.size());
    if (lookupRule(std::string_view(selector.data(), tagName.size() + 1 + cls.size()), matchedStyle)) {
      result.applyOver(matchedStyle);
    }
  });

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (magic + version identify Crossink-owned CSS rule caches)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char rulesCacheTmp[] = "/css_rules.cache.tmp";
constexpr char rulesCacheBackup[] = "/css_rules.cache.bak";

uint32_t CssParser::selectorHash(std::string_view selector) {
  uint32_t h = 2166136261U;
  for (char c : selector) {
    h ^= static_cast<uint8_t>(asciiToLower(c));
    h *= 16777619U;
  }
  return h;
}

bool CssParser::writeCssStylePayload(FsFile& file, const CssStyle& style) {
  auto writeBytes = [&file](const void* data, const size_t len) -> bool {
    return len == 0 || file.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  };
  auto writeByte = [&writeBytes](const uint8_t value) -> bool { return writeBytes(&value, sizeof(value)); };
  auto writeLength = [&writeBytes, &writeByte](const CssLength& len) -> bool {
    return writeBytes(&len.value, sizeof(len.value)) && writeByte(static_cast<uint8_t>(len.unit));
  };

  if (!writeByte(static_cast<uint8_t>(style.textAlign)) || !writeByte(static_cast<uint8_t>(style.fontStyle)) ||
      !writeByte(static_cast<uint8_t>(style.fontWeight)) || !writeByte(static_cast<uint8_t>(style.textDecoration)) ||
      !writeLength(style.textIndent) || !writeLength(style.marginTop) || !writeLength(style.marginBottom) ||
      !writeLength(style.marginLeft) || !writeLength(style.marginRight) || !writeLength(style.paddingTop) ||
      !writeLength(style.paddingBottom) || !writeLength(style.paddingLeft) || !writeLength(style.paddingRight) ||
      !writeLength(style.imageHeight) || !writeLength(style.imageWidth) ||
      !writeByte(static_cast<uint8_t>(style.display)) ||
      !writeByte(static_cast<uint8_t>(style.backgroundBlack ? 1 : 0)) ||
      !writeByte(static_cast<uint8_t>(style.verticalAlign)) || !writeByte(static_cast<uint8_t>(style.direction)) ||
      !writeByte(static_cast<uint8_t>(style.pageBreakBefore ? 1 : 0)) ||
      !writeByte(static_cast<uint8_t>(style.pageBreakAfter ? 1 : 0))) {
    return false;
  }

  uint32_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  if (style.defined.backgroundBlack) definedBits |= 1 << 16;
  if (style.defined.verticalAlign) definedBits |= 1 << 17;
  if (style.defined.direction) definedBits |= 1 << 18;
  if (style.defined.pageBreakBefore) definedBits |= 1 << 20;
  if (style.defined.pageBreakAfter) definedBits |= 1 << 21;
  return writeBytes(&definedBits, sizeof(definedBits));
}

bool CssParser::readCssStylePayload(FsFile& file, CssStyle& style) {
  auto readLength = [&file](CssLength& len) -> bool {
    if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) return false;
    uint8_t unitVal;
    if (file.read(&unitVal, 1) != 1) return false;
    len.unit = static_cast<CssUnit>(unitVal);
    return true;
  };

  uint8_t enumVal;
  if (file.read(&enumVal, 1) != 1) return false;
  style.textAlign = static_cast<CssTextAlign>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.fontStyle = static_cast<CssFontStyle>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.fontWeight = static_cast<CssFontWeight>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.textDecoration = static_cast<CssTextDecoration>(enumVal);
  if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
      !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
      !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
      !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
    return false;
  }
  uint8_t displayVal;
  if (file.read(&displayVal, 1) != 1) return false;
  style.display = static_cast<CssDisplay>(displayVal);
  uint8_t backgroundBlackVal = 0;
  if (file.read(&backgroundBlackVal, 1) != 1) return false;
  style.backgroundBlack = backgroundBlackVal != 0;
  uint8_t verticalAlignVal = 0;
  if (file.read(&verticalAlignVal, 1) != 1) return false;
  style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlignVal);
  uint8_t directionVal = 0;
  if (file.read(&directionVal, 1) != 1) return false;
  style.direction = static_cast<CssTextDirection>(directionVal);
  uint8_t pageBreakVal = 0;
  if (file.read(&pageBreakVal, 1) != 1) return false;
  style.pageBreakBefore = pageBreakVal != 0;
  if (file.read(&pageBreakVal, 1) != 1) return false;
  style.pageBreakAfter = pageBreakVal != 0;

  uint32_t definedBits = 0;
  if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) return false;
  style.defined.textAlign = (definedBits & 1 << 0) != 0;
  style.defined.fontStyle = (definedBits & 1 << 1) != 0;
  style.defined.fontWeight = (definedBits & 1 << 2) != 0;
  style.defined.textDecoration = (definedBits & 1 << 3) != 0;
  style.defined.textIndent = (definedBits & 1 << 4) != 0;
  style.defined.marginTop = (definedBits & 1 << 5) != 0;
  style.defined.marginBottom = (definedBits & 1 << 6) != 0;
  style.defined.marginLeft = (definedBits & 1 << 7) != 0;
  style.defined.marginRight = (definedBits & 1 << 8) != 0;
  style.defined.paddingTop = (definedBits & 1 << 9) != 0;
  style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  style.defined.paddingRight = (definedBits & 1 << 12) != 0;
  style.defined.imageHeight = (definedBits & 1 << 13) != 0;
  style.defined.imageWidth = (definedBits & 1 << 14) != 0;
  style.defined.display = (definedBits & 1 << 15) != 0;
  style.defined.backgroundBlack = (definedBits & 1 << 16) != 0;
  style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
  style.defined.direction = (definedBits & 1 << 18) != 0;
  style.defined.pageBreakBefore = (definedBits & 1 << 20) != 0;
  style.defined.pageBreakAfter = (definedBits & 1 << 21) != 0;
  return true;
}

bool CssParser::readRuleFromDiskAtOffset(const uint32_t ruleOffset, std::string_view selector,
                                         CssStyle& outStyle) const {
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  char selectorBuf[MAX_SELECTOR_LENGTH];
  uint16_t selectorLen = 0;
  bool ok = file.seek(ruleOffset) && file.read(&selectorLen, sizeof(selectorLen)) == sizeof(selectorLen) &&
            selectorLen == selector.size() && selectorLen <= MAX_SELECTOR_LENGTH &&
            file.read(selectorBuf, selectorLen) == selectorLen &&
            SvEqual{}(std::string_view(selectorBuf, selectorLen), selector) && readCssStylePayload(file, outStyle);
  file.close();
  return ok;
}

bool CssParser::lookupRule(std::string_view selector, CssStyle& outStyle) const {
  if (auto it = rulesBySelector_.find(selector); it != rulesBySelector_.end()) {
    outStyle = it->second;
    return true;
  }
  if (selector.empty() || selector.size() > MAX_SELECTOR_LENGTH || !cacheIndexLoaded_) {
    return false;
  }

  const uint32_t h = selectorHash(selector);
  auto it = std::lower_bound(cacheRuleOffsets_.begin(), cacheRuleOffsets_.end(), h,
                             [](const SelectorEntry& e, const uint32_t key) { return e.hash < key; });
  for (; it != cacheRuleOffsets_.end() && it->hash == h; ++it) {
    if (readRuleFromDiskAtOffset(it->offset, selector, outStyle)) {
      return true;
    }
  }
  return false;
}

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  Storage.remove((cachePath + rulesCacheTmp).c_str());
  Storage.remove((cachePath + rulesCacheBackup).c_str());
}

bool CssParser::saveToCache(const bool complete) const {
  if (cachePath.empty()) {
    return false;
  }

  const std::string finalPath = cachePath + rulesCache;
  const std::string tmpPath = cachePath + rulesCacheTmp;
  const std::string backupPath = cachePath + rulesCacheBackup;

  Storage.remove(tmpPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite("CSS", tmpPath, file)) {
    return false;
  }

  bool writeOk = true;
  auto writeBytes = [&file, &writeOk](const void* data, const size_t len) -> bool {
    if (!writeOk) return false;
    if (len == 0) return true;
    if (file.write(reinterpret_cast<const uint8_t*>(data), len) != len) {
      writeOk = false;
    }
    return writeOk;
  };
  auto writeByte = [&writeBytes](const uint8_t value) -> bool { return writeBytes(&value, sizeof(value)); };

  // Write header
  const uint32_t magic = CssParser::CSS_CACHE_MAGIC;
  writeBytes(&magic, sizeof(magic));
  writeByte(CssParser::CSS_CACHE_VERSION);
  writeByte(static_cast<uint8_t>(complete ? 0 : CSS_CACHE_FLAG_PARTIAL));

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  writeBytes(&ruleCount, sizeof(ruleCount));

  std::vector<SelectorEntry> indexEntries;
  indexEntries.reserve(ruleCount);
  const SelectorEntry zeroEntry{0, 0};
  for (uint16_t i = 0; i < ruleCount; ++i) {
    writeBytes(&zeroEntry, sizeof(zeroEntry));
  }

  // Write each simple rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    const uint32_t ruleOffset = file.position();
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    if (!writeBytes(&selectorLen, sizeof(selectorLen)) || !writeBytes(pair.first.data(), selectorLen) ||
        !writeCssStylePayload(file, pair.second)) {
      writeOk = false;
      break;
    }
    indexEntries.push_back({selectorHash(pair.first), ruleOffset});
  }

  // Write descendant rules: count, then (ancestorSelector, subjectSelector, CssStyle) per entry
  const auto descendantCount = static_cast<uint16_t>(descendantRules_.size());
  writeBytes(&descendantCount, sizeof(descendantCount));
  for (const auto& rule : descendantRules_) {
    const auto ancLen = static_cast<uint16_t>(rule.ancestorSelector.size());
    if (!writeBytes(&ancLen, sizeof(ancLen)) || !writeBytes(rule.ancestorSelector.data(), ancLen)) {
      writeOk = false;
      break;
    }
    const auto subLen = static_cast<uint16_t>(rule.subjectSelector.size());
    if (!writeBytes(&subLen, sizeof(subLen)) || !writeBytes(rule.subjectSelector.data(), subLen) ||
        !writeCssStylePayload(file, rule.style)) {
      writeOk = false;
      break;
    }
  }

  if (!writeOk) {
    LOG_ERR("CSS", "Failed to write temporary CSS cache");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  if (!file.seek(sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t))) {
    LOG_ERR("CSS", "Failed to seek CSS index placeholder");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  for (const auto& entry : indexEntries) {
    if (!writeBytes(&entry, sizeof(entry))) {
      break;
    }
  }
  if (!writeOk) {
    LOG_ERR("CSS", "Failed to patch CSS index");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  file.close();

  Storage.remove(backupPath.c_str());
  const bool hadExistingCache = Storage.exists(finalPath.c_str());
  if (hadExistingCache && !Storage.rename(finalPath.c_str(), backupPath.c_str())) {
    LOG_ERR("CSS", "Failed to backup existing CSS cache before replace");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to promote temporary CSS cache");
    Storage.remove(tmpPath.c_str());
    if (hadExistingCache) {
      Storage.rename(backupPath.c_str(), finalPath.c_str());
    }
    return false;
  }

  if (hadExistingCache) {
    Storage.remove(backupPath.c_str());
  }

  LOG_DBG("CSS", "Saved %u rules + %u descendant rules to %s cache", ruleCount, descendantCount,
          complete ? "complete" : "partial");
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }
  struct FileGuard {
    FsFile& f;
    explicit FileGuard(FsFile& f) : f(f) {}
    // Ensure we only close an open file.
    ~FileGuard() {
      if (f.isOpen()) f.close();
    }
  } fileGuard(file);

  // Clear existing rules
  clear();
  cachePartial_ = false;

  // Read and verify header
  uint32_t magic = 0;
  if (file.read(&magic, sizeof(magic)) != sizeof(magic) || magic != CssParser::CSS_CACHE_MAGIC) {
    LOG_DBG("CSS", "Cache magic mismatch, removing stale cache for rebuild");
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint8_t flags = 0;
  if (file.read(&flags, 1) != 1) {
    LOG_DBG("CSS", "Cache flags missing, removing stale cache for rebuild");
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  if ((flags & ~CSS_CACHE_FLAG_PARTIAL) != 0) {
    LOG_DBG("CSS", "Unsupported CSS cache flags 0x%02X, removing stale cache for rebuild", flags);
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  cachePartial_ = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    rulesBySelector_.clear();
    return false;
  }

  auto hasRemainingBytes = [&file](const size_t neededBytes) -> bool {
    return static_cast<size_t>(file.available()) >= neededBytes;
  };

  cacheRuleOffsets_.reserve(ruleCount);
  for (uint16_t i = 0; i < ruleCount; ++i) {
    SelectorEntry entry{};
    if (file.read(&entry, sizeof(entry)) != sizeof(entry)) {
      LOG_DBG("CSS", "Truncated CSS cache while reading selector index");
      cacheRuleOffsets_.clear();
      return false;
    }
    cacheRuleOffsets_.push_back(entry);
  }
  cacheIndexLoaded_ = true;
  cachedRuleCount_ = cacheRuleOffsets_.size();

  // Skip each simple rule payload; simple styles stay disk-backed and are read on demand.
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (!hasRemainingBytes(sizeof(selectorLen)) ||
        file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      cacheRuleOffsets_.clear();
      return false;
    }
    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH ||
        !hasRemainingBytes(static_cast<size_t>(selectorLen) + CSS_FIXED_STYLE_BYTES)) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      cacheRuleOffsets_.clear();
      return false;
    }
    const uint32_t nextRecord = file.position() + selectorLen + CSS_FIXED_STYLE_BYTES;
    if (!file.seek(nextRecord)) {
      cacheRuleOffsets_.clear();
      return false;
    }
  }

  // Read descendant rules
  uint16_t descendantCount = 0;
  if (file.available() > 0) {
    if (file.read(&descendantCount, sizeof(descendantCount)) != sizeof(descendantCount)) {
      LOG_DBG("CSS", "Truncated CSS cache reading descendant count");
      rulesBySelector_.clear();
      return false;
    }
    if (descendantCount > MAX_DESCENDANT_RULES) {
      LOG_DBG("CSS", "Invalid descendant rule count (%u > %zu)", descendantCount, MAX_DESCENDANT_RULES);
      rulesBySelector_.clear();
      return false;
    }
    descendantRules_.reserve(descendantCount);
    for (uint16_t i = 0; i < descendantCount; ++i) {
      auto readStr = [&](std::string& out) -> bool {
        uint16_t len = 0;
        if (file.read(&len, sizeof(len)) != sizeof(len)) return false;
        if (len == 0 || len > MAX_SELECTOR_LENGTH || !hasRemainingBytes(len)) return false;
        out.resize(len);
        return file.read(&out[0], len) == len;
      };
      DescendantRule rule;
      if (!readStr(rule.ancestorSelector) || !readStr(rule.subjectSelector)) {
        LOG_DBG("CSS", "Truncated CSS cache reading descendant rule selectors");
        rulesBySelector_.clear();
        descendantRules_.clear();
        return false;
      }
      if (!hasRemainingBytes(CSS_FIXED_STYLE_BYTES) || !readCssStylePayload(file, rule.style)) {
        LOG_DBG("CSS", "Truncated CSS cache reading descendant rule style");
        rulesBySelector_.clear();
        descendantRules_.clear();
        return false;
      }
      descendantRules_.push_back(std::move(rule));
    }
  }

  LOG_DBG("CSS", "Loaded %u indexed rules + %u descendant rules from %s cache", static_cast<unsigned>(cachedRuleCount_),
          descendantCount, cachePartial_ ? "partial" : "complete");
  return true;
}
