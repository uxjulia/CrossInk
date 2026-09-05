#include "DictionaryDefinitionActivity.h"

#include <BidiUtils.h>
#include <DictHtmlRenderer.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <MemoryBudget.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>

#include "../settings/DictionarySelectActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"
#include "util/TextPool.h"

static constexpr char kBullet[] = "- ";
static constexpr const char kEtymologyTreeMarker[] = "Etymology tree";
static constexpr int kDictionarySwitchTouchHeight = 56;
#if CROSSINK_APP_CAP_TOUCH
static constexpr unsigned long kTouchDefinitionLookupHoldMs = 1000;
#endif

class DictionaryDefinitionActivity;

static std::string dictionaryNameFromPath(const std::string& path) {
  const size_t stemSlash = path.rfind('/');
  if (stemSlash == std::string::npos) return path;

  const size_t nameSlash = stemSlash == 0 ? std::string::npos : path.rfind('/', stemSlash - 1);
  const size_t nameStart = nameSlash == std::string::npos ? 0 : nameSlash + 1;
  return path.substr(nameStart, stemSlash - nameStart);
}

struct DefinitionSpanFeedContext {
  DictionaryDefinitionActivity* activity = nullptr;
  DictLayout::Wrapper* wrapper = nullptr;
  char lastVisibleChar = '\0';
  bool sawEtymologyTree = false;
};

static bool isAsciiWordChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool isGreekCodepoint(uint32_t cp) { return (cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1F00 && cp <= 0x1FFF); }

static bool startsWithAt(const std::string& text, size_t pos, const char* needle) {
  const size_t len = std::char_traits<char>::length(needle);
  return pos + len <= text.size() && text.compare(pos, len, needle) == 0;
}

static bool startsWithAt(const char* text, const char* needle) {
  if (!text) return false;
  const size_t len = std::char_traits<char>::length(needle);
  return strncmp(text, needle, len) == 0;
}

static bool isAsciiUpper(char c) { return c >= 'A' && c <= 'Z'; }

static bool startsWithEtymologyTreeMarker(const char* text) { return startsWithAt(text, kEtymologyTreeMarker); }

static bool definitionTextHasEtymologySpacingArtifact(const char* text, bool inEtymologyTree) {
  if (!text || !text[0]) return false;
  for (const char* p = text; *p; ++p) {
    if (startsWithEtymologyTreeMarker(p)) {
      const char* afterMarker = p + sizeof(kEtymologyTreeMarker) - 1;
      if (*afterMarker != '\0' && *afterMarker != ' ') return true;
    }
    if (inEtymologyTree && *p == '.' && isAsciiUpper(*(p + 1))) return true;
  }
  return false;
}

static void fixEtymologySpacingArtifacts(std::string& text, bool inEtymologyTree) {
  if (text.size() < 2) return;

  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size();) {
    if (startsWithAt(text, i, kEtymologyTreeMarker)) {
      const size_t markerLen = sizeof(kEtymologyTreeMarker) - 1;
      out.append(text, i, markerLen);
      i += markerLen;
      if (i < text.size() && text[i] != ' ') out += ' ';
      inEtymologyTree = true;
      continue;
    }

    if (inEtymologyTree && text[i] == '.' && i + 1 < text.size() && isAsciiUpper(text[i + 1])) {
      out += ". ";
      ++i;
      continue;
    }

    out += text[i++];
  }
  text = std::move(out);
}

static char lastVisibleAsciiChar(const char* text) {
  if (!text) return '\0';
  char last = '\0';
  while (*text) {
    if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n') last = *text;
    ++text;
  }
  return last;
}

static bool shouldSanitizeDefinitionCodepoint(uint32_t cp) {
  switch (cp) {
    case 0x00A0:
    case 0x2013:
    case 0x2014:
    case 0x2212:
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2026:
      return true;
    default:
      return isIpaCodepoint(cp) || isGreekCodepoint(cp) || utf8IsCombiningMark(cp) || cp == REPLACEMENT_GLYPH;
  }
}

// Every Lexend Deca/Bitter reader font header (all sizes and styles) has the
// same direct coverage among the codepoints above. Keep that coverage beside
// the approximation policy instead of probing the font map for every definition.
// The Greek entries below are the renderer's fixed synthetic fallbacks; U+02BB
// is its fixed alias for the left single quotation mark.
static constexpr bool builtinDefinitionFontSupportsCandidate(const uint32_t cp) {
  switch (cp) {
    case 0x00A0:
    case 0x00E6:
    case 0x00F0:
    case 0x00F8:
    case 0x0127:
    case 0x014B:
    case 0x0153:
    case 0x02BB:
    case 0x0393:
    case 0x03B5:
    case 0x03C9:
    case 0x2013:
    case 0x2014:
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2026:
    case 0x2212:
      return true;
    default:
      return (cp >= 0x0300 && cp <= 0x0304) || (cp >= 0x0306 && cp <= 0x030C) || cp == 0x030F ||
             (cp >= 0x0311 && cp <= 0x0312) || cp == 0x031B || (cp >= 0x0323 && cp <= 0x0324) ||
             (cp >= 0x0326 && cp <= 0x0328) || cp == 0x032E || cp == 0x0331 || cp == 0x0335 || cp == 0x034F ||
             cp == 0x03BB;
  }
}

static void appendDictionaryApproximation(uint32_t cp, std::string& out) {
  switch (cp) {
    case 0x00A0:
      out += ' ';
      break;  // nbsp
    case 0x00E6:
      out += "ae";
      break;  // ae
    case 0x00F0:
      out += "th";
      break;  // eth
    case 0x00F8:
      out += 'o';
      break;  // o slash
    case 0x0127:
      out += 'h';
      break;  // h stroke
    case 0x014B:
      out += "ng";
      break;  // eng
    case 0x0153:
      out += "oe";
      break;      // oe
    case 0x0251:  // script a
    case 0x0252:
      out += 'a';
      break;  // turned alpha
    case 0x0254:
      out += 'o';
      break;  // open o
    case 0x0259:
      out += 'e';
      break;      // schwa
    case 0x025A:  // rhotacized schwa
    case 0x025D:
      out += "er";
      break;  // reversed epsilon hook
    case 0x025B:
      out += 'e';
      break;  // open e
    case 0x0261:
      out += 'g';
      break;  // script g
    case 0x026A:
      out += 'i';
      break;  // small capital i
    case 0x026B:
      out += 'l';
      break;  // velarized l
    case 0x0272:
      out += "ny";
      break;  // n hook
    case 0x0273:
      out += 'n';
      break;      // retroflex n
    case 0x0279:  // turned r
    case 0x027B:  // turned r hook
    case 0x027D:  // r tail
    case 0x027E:
      out += 'r';
      break;  // tap r
    case 0x0280:
      out += 'R';
      break;  // small capital r
    case 0x0283:
      out += "sh";
      break;  // esh
    case 0x028A:
      out += 'u';
      break;  // upsilon
    case 0x028C:
      out += 'u';
      break;  // turned v
    case 0x0292:
      out += "zh";
      break;  // ezh
    case 0x0294:
      out += '\'';
      break;  // glottal stop
    case 0x02A4:
      out += 'j';
      break;  // dezh digraph
    case 0x02A7:
      out += "ch";
      break;  // tesh digraph
    case 0x02B0:
      out += 'h';
      break;  // modifier h
    case 0x02B2:
      out += 'y';
      break;  // modifier j
    case 0x02B7:
      out += 'w';
      break;  // modifier w
    case 0x02BC:
      out += '\'';
      break;      // modifier apostrophe
    case 0x02C8:  // primary stress
    case 0x02CC:
      break;  // secondary stress
    case 0x02D0:
      out += ':';
      break;  // length mark
    case 0x03B2:
      out += 'b';
      break;  // beta
    case 0x03B8:
      out += "th";
      break;  // theta
    case 0x03C7:
      out += "kh";
      break;      // chi
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2212:
      out += '-';
      break;      // minus
    case 0x2018:  // left single quote
    case 0x2019:
      out += '\'';
      break;      // right single quote
    case 0x201C:  // left double quote
    case 0x201D:
      out += '"';
      break;  // right double quote
    case 0x2026:
      out += "...";
      break;  // ellipsis
    default:
      if (!isIpaCodepoint(cp) && !isGreekCodepoint(cp) && !utf8IsCombiningMark(cp) && cp != REPLACEMENT_GLYPH) {
        utf8AppendCodepoint(cp, out);
      }
      break;
  }
}

static bool startsWithCapitalizedToken(const char* text) { return text && isAsciiUpper(text[0]); }

static bool definitionTextMayNeedBidi(const char* text) {
  if (!text) return false;
  while (*text) {
    const auto b = static_cast<unsigned char>(*text++);
    if (b >= 0xD6 && b <= 0xDB) return true;  // Hebrew and Arabic UTF-8 lead bytes
  }
  return false;
}

static EpdFontFamily::Style styleForSpan(const StyledSpan& span) {
  if (span.bold && span.italic) return EpdFontFamily::BOLD_ITALIC;
  if (span.bold) return EpdFontFamily::BOLD;
  if (span.italic) return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  const DictionaryFontActivation activation =
      sdFontSystem.activateDictionaryFont(renderer, dictionaryFontFamilyName_, dictionaryFontPointSize_);
  definitionFontId_ = activation.fontId;
  definitionFontSource_ =
      activation.usingDictionaryFont ? DefinitionFontSource::Dictionary : DefinitionFontSource::Reader;
  if (renderer.isSdCardFont(definitionFontId_)) {
    // Dictionary preparation retains advances but deliberately starts without
    // reader kerning/page caches; the lean policy below reloads ligatures and
    // exact visible glyph/style data only.
    renderer.releaseSdCardFontForLowMemory(definitionFontId_, /*preserveAdvanceTable=*/true);
  }
  wrapText();
  requestUpdate();
  // SD write overlaps the e-ink refresh kicked by requestUpdate() on the render task.
  LookupHistory::addWordIf(cachePath, historyWord, historyStatus, recordHistory);

  // Seed the back-nav chain. The initial word is the newest history entry iff it
  // was just logged (same condition addWordIf applies internally).
  chain_.reset(LookupHistory::MAX_VISIBLE_ENTRIES);
  const bool initialLogged = recordHistory && !historyWord.empty() && !cachePath.empty();
  chain_.setCurrentHistIndex(initialLogged ? 0 : -1);
}

void DictionaryDefinitionActivity::onExit() {
  controller.onExit();
  Dictionary::clearLookupDictPathOverride();
  if (renderer.isSdCardFont(definitionFontId_)) {
    // The dictionary can reuse the reader's family and size, in which case
    // restoreReaderFont() does not switch IDs. Release the definition caches
    // explicitly so that path does not retain dictionary-only glyph data.
    renderer.releaseSdCardFontForLowMemory(definitionFontId_);
  }
  sdFontSystem.restoreReaderFont(renderer);
  Activity::onExit();
}

int DictionaryDefinitionActivity::getDefinitionFontId(bool) const {
  return definitionFontId_ != 0 ? definitionFontId_ : SETTINGS.getReaderFontId();
}

void DictionaryDefinitionActivity::useBuiltInDefinitionFontFallback() {
  const int failedFontId = getDefinitionFontId();
  if (definitionFontSource_ == DefinitionFontSource::Dictionary) {
    definitionFontId_ = sdFontSystem.restoreReaderFont(renderer);
    definitionFontSource_ = DefinitionFontSource::Reader;
    LOG_ERR("DICT", "Dictionary SD font %d failed to prepare; retrying with reader font %d", failedFontId,
            definitionFontId_);
  } else if (definitionFontSource_ == DefinitionFontSource::Reader) {
    definitionFontId_ = SETTINGS.getBuiltInReaderFontId();
    definitionFontSource_ = DefinitionFontSource::BuiltIn;
    LOG_ERR("DICT", "Reader SD font %d failed to prepare dictionary glyphs; using built-in font %d", failedFontId,
            definitionFontId_);
  } else {
    return;
  }
  reflowForDefinitionFontChange();
  // The failed font was the active owner of the definition bitmap cache. Its
  // family may also have replaced the reader family while the modal background
  // was prepared, so redraw the page with the restored reader font before the
  // fallback definition is rendered. This reuses the existing framebuffer;
  // retaining the old pixels produces replacement diamonds behind a blank
  // modal after a large dictionary-font prewarm fails.
  if (hasModalBackground()) {
    modalBackgroundNeedsRedraw_ = true;
    modalCleanRefreshNeeded_ = true;
  }
}

void DictionaryDefinitionActivity::reflowForDefinitionFontChange() {
  const int requestedPage = currentPage;
  wrapText();
  currentPage = std::min(requestedPage, std::max(0, totalPages - 1));
  if (currentPage > 0) loadPage(currentPage);
  if (hasModalBackground()) sizeModalForCurrentPage();
}

void DictionaryDefinitionActivity::redrawModalBackground() {
  if (!hasModalBackground() || !modalBackgroundNeedsRedraw_) return;

  if (definitionFontSource_ == DefinitionFontSource::Dictionary) {
    sdFontSystem.restoreReaderFont(renderer);
    backgroundRender_(backgroundContext_);
    const DictionaryFontActivation activation =
        sdFontSystem.activateDictionaryFont(renderer, dictionaryFontFamilyName_, dictionaryFontPointSize_);
    definitionFontId_ = activation.fontId;
    definitionFontSource_ =
        activation.usingDictionaryFont ? DefinitionFontSource::Dictionary : DefinitionFontSource::Reader;
    if (renderer.isSdCardFont(definitionFontId_)) {
      renderer.releaseSdCardFontForLowMemory(definitionFontId_, /*preserveAdvanceTable=*/true);
    }
    // SD font IDs are deterministic, so reloading the same family returns the
    // same ID even though its advance table was discarded with the old font.
    // Reflow every restored dictionary font to rebuild those metrics before
    // drawing; otherwise lines wrapped with stale/narrow widths can overflow.
    reflowForDefinitionFontChange();
  } else {
    backgroundRender_(backgroundContext_);
  }
  modalBackgroundNeedsRedraw_ = false;
}

void DictionaryDefinitionActivity::displayModalBuffer() {
  renderer.displayBuffer(modalCleanRefreshNeeded_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  modalCleanRefreshNeeded_ = false;
}

bool DictionaryDefinitionActivity::shouldApproximateDefinitionCodepoint(const uint32_t cp) const {
  if (!shouldSanitizeDefinitionCodepoint(cp)) return false;

  const int fontId = getDefinitionFontId();
  // SD fonts own their coverage. Do not replace pronunciation, Greek, or combining
  // characters before the active .cpfont gets a chance to draw them.
  if (renderer.isSdCardFont(fontId)) return false;

  return !builtinDefinitionFontSupportsCandidate(cp);
}

bool DictionaryDefinitionActivity::definitionTextNeedsApproximation(const char* text) const {
  if (!text) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(&p))) {
    if (shouldApproximateDefinitionCodepoint(cp)) return true;
  }
  return false;
}

std::string DictionaryDefinitionActivity::approximateDefinitionText(const char* text,
                                                                    const bool inEtymologyTree) const {
  std::string out;
  if (!text) return out;
  out.reserve(std::char_traits<char>::length(text));

  const auto* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(&p))) {
    if (shouldApproximateDefinitionCodepoint(cp)) {
      appendDictionaryApproximation(cp, out);
    } else {
      utf8AppendCodepoint(cp, out);
    }
  }
  fixEtymologySpacingArtifacts(out, inEtymologyTree);
  return out;
}

int DictionaryDefinitionActivity::getLineHeight() const {
  return static_cast<int>(renderer.getLineHeight(getDefinitionFontId()) * SETTINGS.getReaderLineCompression());
}

#if CROSSINK_APP_CAP_TOUCH
bool DictionaryDefinitionActivity::showTouchDictionarySwitch() const {
  return hasModalBackground() && showLookupButton && mappedInput.hasTouch();
}
#endif

int DictionaryDefinitionActivity::dictionaryFooterHeight() const {
  if (!hasModalBackground()) return 0;

  const auto metrics = UITheme::getInstance().getMetrics();
  const int dictionaryNameHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.optionPopupTitleGap;
#if CROSSINK_APP_CAP_TOUCH
  return dictionaryNameHeight +
         (showTouchDictionarySwitch() ? kDictionarySwitchTouchHeight * (hasClippingRequest_ ? 2 : 1) : 0);
#else
  return dictionaryNameHeight;
#endif
}

#if CROSSINK_APP_CAP_TOUCH
bool DictionaryDefinitionActivity::dictionarySwitchButtonContains(const int x, const int y) const {
  const int buttonY = modalY_ + modalHeight_ - kDictionarySwitchTouchHeight * (hasClippingRequest_ ? 2 : 1);
  return x >= modalX_ && x < modalX_ + modalWidth_ && y >= buttonY && y < buttonY + kDictionarySwitchTouchHeight;
}

bool DictionaryDefinitionActivity::dictionaryCreateClippingButtonContains(const int x, const int y) const {
  const int buttonY = modalY_ + modalHeight_ - kDictionarySwitchTouchHeight;
  return hasClippingRequest_ && x >= modalX_ && x < modalX_ + modalWidth_ && y >= buttonY &&
         y < buttonY + kDictionarySwitchTouchHeight;
}

bool DictionaryDefinitionActivity::modalContains(const int x, const int y) const {
  const int frameThickness = UITheme::getInstance().getMetrics().popupFrameThickness;
  return x >= modalX_ - frameThickness && x < modalX_ + modalWidth_ + frameThickness && y >= modalY_ - frameThickness &&
         y < modalY_ + modalHeight_ + frameThickness;
}
#endif

// ---------------------------------------------------------------------------
// Layout helpers — shared setup
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapText() {
  isWordSelectMode = false;
  navigator.reset();
  currentPage = 0;  // new definition always starts at page 0
  // The initial reader page is already present when opened from word-select.
  // Chained definitions keep the existing modal covered by growing its frame,
  // so they do not need to swap back to the reader font for a background draw.
  if (hasModalBackground()) {
    if (skipInitialModalBackgroundRedraw_) {
      modalBackgroundNeedsRedraw_ = false;
      skipInitialModalBackgroundRedraw_ = false;
    }
  }

  // Match the folder-name label shown by DictionarySelectActivity.
  dictionaryName_ = dictionaryNameFromPath(foundLocation.folderPath);

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.buttonHintsHeight : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  if (hasModalBackground()) {
    const int dialogMargin = metrics.optionPopupDialogSideMargin;
    // Reserve the orientation hint gutter on both sides. The controls only
    // occupy one side, but symmetrical gutters keep the modal centered.
    modalX_ = hintGutterWidth + dialogMargin;
    modalWidth_ = renderer.getScreenWidth() - modalX_ * 2;
    modalHeight_ = renderer.getScreenHeight() - bottomArea - dialogMargin * 2;
    modalY_ = (renderer.getScreenHeight() - modalHeight_) / 2;

    const int innerPadding = metrics.optionPopupInnerPadding;
    leftPadding = modalX_ + innerPadding;
    rightPadding = renderer.getScreenWidth() - (modalX_ + modalWidth_ - innerPadding);
    bodyStartY = modalY_ + innerPadding + renderer.getLineHeight(getDefinitionFontId()) + metrics.optionPopupTitleGap;
  } else {
    const int sidePadding = metrics.contentSidePadding + SETTINGS.screenMarginHorizontal;
    leftPadding = contentX + sidePadding;
    rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
    bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  }

  const int footerHeight = dictionaryFooterHeight();
  const int bodyBottom = hasModalBackground() ? modalY_ + modalHeight_ - metrics.optionPopupInnerPadding - footerHeight
                                              : renderer.getScreenHeight() - bottomArea;
  linesPerPage = (bodyBottom - bodyStartY) / getLineHeight();
  if (linesPerPage < 1) linesPerPage = 1;

  // Resolve the immutable definition slice once. Page turns reuse it instead
  // of reopening the dictionary metadata files before every layout pass.
  const DictInfo info = Dictionary::readInfo(foundLocation.folderPath.c_str());
  const DictDefinitionSlice slice = Dictionary::resolveDefinitionSlice(foundLocation, info);
  definitionReadFailed_ = !slice.found;
  if (definitionReadFailed_) {
    LOG_ERR("DICT", "Failed to resolve definition slice for %s", foundLocation.folderPath.c_str());
  }
  definitionOffset_ = slice.offset;
  definitionSize_ = slice.size;
  definitionIsHtml_ = slice.isHtml;
  definitionHtmlNeedsPlainFallback_ = false;

  prepareDefinitionFontAdvances();
  loadPage(currentPage);
  // Pick the modal height once per definition. Page turns re-fill the content
  // without moving the frame, so the last (usually shorter) page feels like
  // part of the same lookup rather than a different modal.
  if (hasModalBackground()) sizeModalForCurrentPage();
}

void DictionaryDefinitionActivity::drawModalFrame() const {
  const auto metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int frameRadius = metrics.popupCornerRadius;

  if (frameRadius > 0) {
    renderer.fillRoundedRect(modalX_ - frameThickness, modalY_ - frameThickness, modalWidth_ + frameThickness * 2,
                             modalHeight_ + frameThickness * 2, frameRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(modalX_, modalY_, modalWidth_, modalHeight_, frameRadius, Color::Black);
    renderer.fillRoundedRect(modalX_ + frameThickness, modalY_ + frameThickness, modalWidth_ - frameThickness * 2,
                             modalHeight_ - frameThickness * 2, std::max(0, frameRadius - frameThickness),
                             Color::White);
  } else {
    renderer.fillRect(modalX_ - frameThickness, modalY_ - frameThickness, modalWidth_ + frameThickness * 2,
                      modalHeight_ + frameThickness * 2, true);
    renderer.fillRect(modalX_, modalY_, modalWidth_, modalHeight_, false);
  }
}

// Re-parse the definition and lay out ONLY `page` into layoutLines. The wrap
// produces every line, but collectLineSink keeps only this page's lines (the
// rest are produced then dropped, so peak RAM is one page, not the whole
// definition) and counts all lines to recompute totalPages. Called on entry and
// on every page turn (Stage 2a: re-parse every turn, both directions).
void DictionaryDefinitionActivity::loadPage(int page) {
  MemoryBudget::logHeapShape("dict.page_before");
  layoutLines.clear();
  layoutLines.reserve(static_cast<size_t>(linesPerPage) + 1);
  layoutSegments.clear();
  layoutSegments.reserve(static_cast<size_t>(linesPerPage) * 4);
  pagePool_.clear();
  collectTargetPage_ = page;
  collectLineCount_ = 0;

  if (definitionReadFailed_) {
    totalPages = 1;
    return;
  }

  if (definitionIsHtml_) {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = DictLayout::paginate(collectLineCount_, linesPerPage);
  MemoryBudget::logHeapShape("dict.page_after");
}

void DictionaryDefinitionActivity::collectDefinitionAdvanceText(const char* text, const EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  definitionAdvanceStyleMask_ |= static_cast<uint8_t>(1u << baseStyle);

  const auto collect = [this](const char* utf8) {
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    uint32_t cp = 0;
    while ((cp = utf8NextCodepoint(&p))) {
      const auto begin = definitionAdvanceCodepoints_.begin();
      const auto end = begin + definitionAdvanceCodepointCount_;
      if (std::find(begin, end, cp) != end) continue;
      if (definitionAdvanceCodepointCount_ >= definitionAdvanceCodepoints_.size()) {
        definitionAdvanceCodepointsTruncated_ = true;
        continue;
      }
      definitionAdvanceCodepoints_[definitionAdvanceCodepointCount_++] = cp;
    }
  };

  collect(text);
  if (definitionTextMayNeedBidi(text) &&
      BidiUtils::applyBidiVisual(text, definitionAdvanceVisualText_, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
    collect(definitionAdvanceVisualText_.c_str());
  }
}

void DictionaryDefinitionActivity::collectSpanForAdvances(void* ctx, const StyledSpan& span) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  self->collectDefinitionAdvanceText(span.text, styleForSpan(span));
}

void DictionaryDefinitionActivity::prepareDefinitionFontAdvances() {
  const int fontId = getDefinitionFontId();
  if (!renderer.isSdCardFont(fontId) || definitionSize_ == 0) return;

  definitionAdvanceCodepointCount_ = 0;
  definitionAdvanceStyleMask_ = 0;
  definitionAdvanceCodepointsTruncated_ = false;
  definitionAdvanceVisualText_.clear();

  // The title is measured before the bitmap prewarm scan, so include it in the
  // same batched advance read as the definition body.
  collectDefinitionAdvanceText(headword.c_str(), EpdFontFamily::BOLD);
  collectDefinitionAdvanceText(kBullet, EpdFontFamily::REGULAR);

  const std::string dictPath = foundLocation.folderPath + ".dict";
  if (definitionIsHtml_) {
    const DictHtmlRenderer::SpanSink sink{this, &DictionaryDefinitionActivity::collectSpanForAdvances};
    if (!htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), definitionOffset_, definitionSize_, sink)) {
      definitionHtmlNeedsPlainFallback_ = true;
      LOG_ERR("DICT", "Malformed HTML definition; collecting SD-font advances from plain text");
      if (!htmlRenderer_.renderPlainTextFromFileStreaming(dictPath.c_str(), definitionOffset_, definitionSize_, sink)) {
        LOG_ERR("DICT", "Failed to collect SD-font advances from definition fallback");
        return;
      }
    }
  } else {
    HalFile dictFile;
    if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) {
      LOG_ERR("DICT", "Failed to open definition for SD-font advance preparation");
      return;
    }
    if (!dictFile.seekSet(definitionOffset_)) {
      LOG_ERR("DICT", "Failed to seek definition for SD-font advance preparation");
      dictFile.close();
      return;
    }

    // Keep only one word while streaming so UTF-8 sequences split across file
    // chunks remain intact without retaining the full definition.
    std::string word;
    word.reserve(64);
    uint32_t remaining = definitionSize_;
    char chunk[512];
    while (remaining > 0) {
      const uint32_t toRead = std::min<uint32_t>(remaining, sizeof(chunk));
      const int n = dictFile.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
      if (n <= 0) break;
      remaining -= static_cast<uint32_t>(n);
      for (int i = 0; i < n; i++) {
        const char c = chunk[i];
        if (c == '\0' || c == '\n' || c == ' ') {
          collectDefinitionAdvanceText(word.c_str(), EpdFontFamily::REGULAR);
          word.clear();
        } else {
          word += c;
        }
      }
    }
    collectDefinitionAdvanceText(word.c_str(), EpdFontFamily::REGULAR);
    dictFile.close();
  }

  if (definitionAdvanceCodepointCount_ == 0) return;
  if (definitionAdvanceCodepointsTruncated_) {
    LOG_DBG("DICT", "SD-font advance preparation capped at %u unique codepoints", kDefinitionAdvanceCodepointCapacity);
  }

  // The .dict reader is closed above. Batch the sorted font reads now so the
  // wrapping pass below performs RAM lookups instead of opening .cpfont once
  // per character while streaming the dictionary.
  LOG_DBG("DICT", "Preparing %u SD-font advance codepoints (styles=0x%02X)", definitionAdvanceCodepointCount_,
          definitionAdvanceStyleMask_);
  renderer.ensureSdCardFontReady(fontId, definitionAdvanceCodepoints_.data(), definitionAdvanceCodepointCount_,
                                 /*includeSpace=*/true, /*includeHyphen=*/false, definitionAdvanceStyleMask_);
}

void DictionaryDefinitionActivity::sizeModalForCurrentPage() {
  const auto metrics = UITheme::getInstance().getMetrics();
  const int dialogMargin = metrics.optionPopupDialogSideMargin;
  const int maxHeight =
      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - dialogMargin * 2;
  const int visibleLines = std::max(1, static_cast<int>(layoutLines.size()));
  const int titleLineHeight = renderer.getLineHeight(getDefinitionFontId());
  const int footerHeight = dictionaryFooterHeight();
  const int contentHeight = metrics.optionPopupInnerPadding * 2 + titleLineHeight + metrics.optionPopupTitleGap +
                            visibleLines * getLineHeight() + footerHeight;

  const int desiredHeight = std::min(maxHeight, contentHeight);
  const int retainedHeight = modalSessionHeight_;
  if (retainedHeight > 0 && desiredHeight > retainedHeight) {
    // The new outer frame erases the old black border. A clean waveform keeps
    // that erased border from appearing as a second modal on the physical panel.
    modalCleanRefreshNeeded_ = true;
  }
  modalSessionHeight_ = std::max(retainedHeight, desiredHeight);
  modalHeight_ = modalSessionHeight_;
  modalY_ = (renderer.getScreenHeight() - modalHeight_) / 2;
  bodyStartY = modalY_ + metrics.optionPopupInnerPadding + titleLineHeight + metrics.optionPopupTitleGap;
  LOG_DBG("DICT", "Modal height desired=%d retained=%d final=%d", desiredHeight, retainedHeight, modalHeight_);
}

void DictionaryDefinitionActivity::collectLineSink(void* ctx, const DictLayout::LayoutLineView& line) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int idx = self->collectLineCount_++;
  const int start = self->collectTargetPage_ * self->linesPerPage;
  if (idx < start || idx >= start + self->linesPerPage) return;  // not on this page — discard

  // Pool the kept line's text: each (already same-style-merged) segment becomes
  // one null-terminated pool entry referenced by {offset, len}.
  PooledLine pooled;
  pooled.indentLevel = line.indentLevel;
  pooled.isListItem = line.isListItem;
  pooled.firstSegment = static_cast<uint16_t>(self->layoutSegments.size());
  pooled.segmentCount = line.segmentCount;
  for (uint16_t i = 0; i < line.segmentCount; ++i) {
    const auto& seg = line.segments[i];
    PooledSegment ps;
    ps.offset = TextPool::append(self->pagePool_, line.textPool + seg.offset, seg.length);
    ps.len = seg.length;
    ps.style = seg.style;
    ps.isIpa = seg.isIpa;
    self->layoutSegments.push_back(ps);
  }
  self->layoutLines.push_back(std::move(pooled));
}

// ---------------------------------------------------------------------------
// Shared helper: measure text width accounting for mixed IPA/non-IPA runs
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text,
                                                EpdFontFamily::Style style) {
  ipaRuns.clear();
  splitIpaRuns(text, ipaRuns);
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + renderer.getTextWidth(getDefinitionFontId(run.isIpa), run.text.c_str(), style);
  });
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isIpa) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = self->getDefinitionFontId(isIpa);
  if (!isIpa && text[0] == ' ' && text[1] == '\0') return self->renderer.getSpaceWidth(fontId, style);
  return self->renderer.getTextWidth(fontId, text, style);
}

void DictionaryDefinitionActivity::wrapHtml() {
  const int maxWidth = renderer.getScreenWidth() - leftPadding - rightPadding;
  // Indent step: 3 spaces worth of pixels at regular weight.
  const int bodyFontId = getDefinitionFontId();
  const int indentStep = renderer.getTextWidth(bodyFontId, "   ");
  const int bulletWidth = renderer.getTextWidth(bodyFontId, kBullet);

  // Fully streamed: the renderer delivers spans one at a time to the Wrapper, the
  // Wrapper emits completed lines to the page collector, and the collector keeps
  // only the current page. Neither the whole-definition span/textBuf (renderer)
  // nor all pages of lines (here) is ever materialized.
  const std::string dictPath = foundLocation.folderPath + ".dict";
  const auto streamIntoLayout = [&](const bool plainTextFallback) {
    DictLayout::Measurer measure{this, &DictionaryDefinitionActivity::measureWidthAdapter};
    DictLayout::LineSink lineSink{this, &DictionaryDefinitionActivity::collectLineSink};
    DictLayout::Wrapper wrapper(DictLayout::WrapMetrics{maxWidth, indentStep, bulletWidth}, measure, lineSink);
    DefinitionSpanFeedContext feedCtx{this, &wrapper};
    const DictHtmlRenderer::SpanSink spanSink{&feedCtx, &DictionaryDefinitionActivity::feedSpanToWrapper};
    const bool ok = plainTextFallback ? htmlRenderer_.renderPlainTextFromFileStreaming(
                                            dictPath.c_str(), definitionOffset_, definitionSize_, spanSink)
                                      : htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), definitionOffset_,
                                                                              definitionSize_, spanSink);
    wrapper.finish();
    return ok;
  };

  // Both paths reset and stream one span at a time, so recovery never
  // materializes the full definition. Once strict parsing fails, page turns
  // reuse the known-good fallback without repeating the failed parse.
  if (!definitionHtmlNeedsPlainFallback_ && !streamIntoLayout(false)) {
    definitionHtmlNeedsPlainFallback_ = true;
    LOG_ERR("DICT", "Malformed HTML definition; displaying plain text");
  }
  if (definitionHtmlNeedsPlainFallback_) {
    // The strict pass may already have emitted the valid prefix. Reset the page
    // collector before replaying the complete definition as plain text.
    layoutLines.clear();
    layoutSegments.clear();
    pagePool_.clear();
    collectLineCount_ = 0;
    if (!streamIntoLayout(true)) {
      LOG_ERR("DICT", "Failed to read definition plain-text fallback");
      definitionReadFailed_ = true;
    }
  }
  // Only the kept page's span text was ever copied into layoutLines.
}

void DictionaryDefinitionActivity::feedSpanToWrapper(void* ctx, const StyledSpan& span) {
  auto* feedCtx = static_cast<DefinitionSpanFeedContext*>(ctx);
  const bool hasMarker = span.text && strstr(span.text, kEtymologyTreeMarker) != nullptr;
  const bool needsBoundarySpace = feedCtx->sawEtymologyTree && feedCtx->lastVisibleChar != '\0' &&
                                  (isAsciiWordChar(feedCtx->lastVisibleChar) || feedCtx->lastVisibleChar == '.') &&
                                  startsWithCapitalizedToken(span.text);
  const bool needsCleanup = needsBoundarySpace || feedCtx->activity->definitionTextNeedsApproximation(span.text) ||
                            definitionTextHasEtymologySpacingArtifact(span.text, feedCtx->sawEtymologyTree);
  if (!needsCleanup) {
    feedCtx->wrapper->onSpan(span);
    feedCtx->lastVisibleChar = lastVisibleAsciiChar(span.text);
    feedCtx->sawEtymologyTree = feedCtx->sawEtymologyTree || hasMarker;
    return;
  }

  std::string cleaned = feedCtx->activity->approximateDefinitionText(span.text, feedCtx->sawEtymologyTree);
  if (needsBoundarySpace && !cleaned.empty() && cleaned[0] != ' ') {
    cleaned.insert(cleaned.begin(), ' ');
  }
  if (cleaned.empty()) return;

  StyledSpan cleanedSpan = span;
  cleanedSpan.text = cleaned.c_str();
  feedCtx->wrapper->onSpan(cleanedSpan);
  feedCtx->lastVisibleChar = lastVisibleAsciiChar(cleaned.c_str());
  feedCtx->sawEtymologyTree = feedCtx->sawEtymologyTree || hasMarker;
}

// ---------------------------------------------------------------------------
// Plain text path: word-wrap into single-segment REGULAR lines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapPlain() {
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  std::string currentWord;
  currentWord.reserve(64);

  DictLayout::Measurer measure{this, &DictionaryDefinitionActivity::measureWidthAdapter};
  DictLayout::LineSink sink{this, &DictionaryDefinitionActivity::collectLineSink};
  DictLayout::Wrapper wrapper(DictLayout::WrapMetrics{maxWidth, 0, 0}, measure, sink);

  auto feedWord = [&]() {
    if (currentWord.empty()) return;
    std::string word = (definitionTextNeedsApproximation(currentWord.c_str()) ||
                        definitionTextHasEtymologySpacingArtifact(currentWord.c_str(), false))
                           ? approximateDefinitionText(currentWord.c_str(), false)
                           : std::move(currentWord);
    currentWord.clear();
    if (word.empty()) return;

    const StyledSpan span{.text = word.c_str()};
    wrapper.onSpan(span);
  };

  const StyledSpan space{.text = " "};

  // Stream from .dict file — the full definition is never held in RAM.
  const std::string dictPath = foundLocation.folderPath + ".dict";
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) {
    LOG_ERR("DICT", "Failed to open definition %s", dictPath.c_str());
    definitionReadFailed_ = true;
    return;
  }
  if (!dictFile.seekSet(definitionOffset_)) {
    LOG_ERR("DICT", "Failed to seek definition %s", dictPath.c_str());
    definitionReadFailed_ = true;
    dictFile.close();
    return;
  }

  uint32_t remaining = definitionSize_;
  char chunk[512];

  while (remaining > 0) {
    uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
    int n = dictFile.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
    if (n <= 0) {
      LOG_ERR("DICT", "Failed reading definition %s", dictPath.c_str());
      definitionReadFailed_ = true;
      break;
    }
    remaining -= static_cast<uint32_t>(n);

    for (int ci = 0; ci < n; ci++) {
      char c = chunk[ci];
      if (c == '\0') {
        feedWord();
        wrapper.onSpan(space);
      } else if (c == '\n') {
        feedWord();
        wrapper.lineBreak();
      } else if (c == ' ') {
        feedWord();
        wrapper.onSpan(space);
      } else {
        currentWord += c;
      }
    }
  }

  feedWord();
  wrapper.finish();
  dictFile.close();
}

// ---------------------------------------------------------------------------
// Word-select: extract words from the currently visible page
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::extractWordsFromLayout() {
  const int bodyFontId = getDefinitionFontId();
  const int indentStep = renderer.getTextWidth(bodyFontId, "   ");

  std::vector<WordSelectNavigator::WordInfo> words;
  words.reserve(64);
  std::vector<WordSelectNavigator::Row> rows;
  rows.reserve(16);
  std::string textPool;
  textPool.reserve(512);

  const int lineHeight = getLineHeight();  // cached for loop
  for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
    const PooledLine& line = layoutLines[i];
    const int16_t lineY = static_cast<int16_t>(bodyStartY + i * lineHeight);
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      x += renderer.getTextWidth(bodyFontId, kBullet);
    }

    for (uint16_t segmentIdx = 0; segmentIdx < line.segmentCount; ++segmentIdx) {
      const PooledSegment& seg = layoutSegments[line.firstSegment + segmentIdx];
      const int segFontId = getDefinitionFontId(seg.isIpa);
      const int spaceWidth = renderer.getSpaceWidth(segFontId, seg.style);
      const char* p = pagePool_.data() + seg.offset;
      while (*p) {
        while (*p == ' ') {
          x += spaceWidth;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        const size_t tokLen = static_cast<size_t>(p - tokStart);
        std::string tok(tokStart, tokLen);

        const int tokAdvanceX = renderer.getTextAdvanceX(segFontId, tok.c_str(), seg.style);
        std::string cleaned = Dictionary::cleanWord(tok);
        if (!cleaned.empty()) {
          uint16_t tokOff = WordSelectNavigator::poolAppend(textPool, tok.c_str(), tok.size());
          uint16_t cleanedOff = WordSelectNavigator::poolAppend(textPool, cleaned.c_str(), cleaned.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = tokOff;
          wi.textLen = static_cast<uint16_t>(tok.size());
          wi.lookupOffset = cleanedOff;
          wi.lookupLen = static_cast<uint16_t>(cleaned.size());
          wi.screenX = static_cast<int16_t>(x);
          wi.screenY = lineY;
          wi.width = static_cast<int16_t>(tokAdvanceX);
          wi.style = seg.style;
          wi.isIpa = seg.isIpa;
          wi.fontId = segFontId;
          words.push_back(wi);
        }
        x += tokAdvanceX;
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
  navigator.load(std::move(words), std::move(rows), std::move(textPool));
}

void DictionaryDefinitionActivity::openDictionarySwitch() {
  if (hasModalBackground()) {
    // Capture the exact frame currently on the panel before the picker covers
    // it. This is the hard lower bound for every replacement definition, even
    // if its content or active font would naturally produce a shorter dialog.
    modalSessionHeight_ = std::max(modalSessionHeight_, modalHeight_);
    LOG_DBG("DICT", "Dictionary switch retaining modal height=%d", modalSessionHeight_);
  }
  auto picker = makeUniqueNoThrow<DictionarySelectActivity>(renderer, mappedInput, cachePath, true, true);
  if (!picker) {
    LOG_ERR("DICT", "OOM: DictionarySelectActivity");
    return;
  }

  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    // The picker replaced the full framebuffer. Its result handler runs before
    // this activity is queued for repaint, so restore the reader background on
    // that next frame whether the selection changed or was cancelled.
    modalBackgroundNeedsRedraw_ = true;
    modalCleanRefreshNeeded_ = true;
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto* selection = std::get_if<FilePathResult>(&result.data);
    if (!selection) {
      LOG_ERR("DICT", "Dictionary switch returned no path");
      requestUpdate();
      return;
    }
    Dictionary::setLookupDictPathOverride(selection->path.c_str());
    dictionaryName_ = dictionaryNameFromPath(selection->path);
    dictionarySwitchLookupInProgress = true;
    controller.startLookup(headword, false);
  });
}

// ---------------------------------------------------------------------------
// Input loop
// ---------------------------------------------------------------------------

bool DictionaryDefinitionActivity::handleLongPressExitAll(bool enabled) {
  if (exitAllOnBackRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      setResult(ActivityResult{});
      finish();
    }
    return true;
  }

  if (enabled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    exitAllOnBackRelease_ = true;
    return true;
  }
  return false;
}

bool DictionaryDefinitionActivity::enterWordSelectMode() {
  if (!hasSharedHighlightSnapshotStorage_ && !ownedHighlightSnapshotStorage_) {
    // Fixed 4 KB activity-lifetime buffer: too large for the task stack and
    // only needed when history-launched definitions enter word selection.
    ownedHighlightSnapshotStorage_ = makeUniqueNoThrow<WordSelectNavigator::HighlightSnapshotStorage>();
    if (!ownedHighlightSnapshotStorage_) {
      LOG_ERR("DICT", "OOM allocating definition highlight snapshot (%u bytes)",
              static_cast<unsigned>(sizeof(WordSelectNavigator::HighlightSnapshotStorage)));
    } else {
      navigator.setHighlightSnapshotStorage(ownedHighlightSnapshotStorage_.get());
    }
  }
  extractWordsFromLayout();
  if (navigator.isEmpty()) return false;

#if CROSSINK_APP_CAP_TOUCH
  navigator.setTouchDragCursorVisible(mappedInput.hasTouch());
#endif
  isWordSelectMode = true;
  return true;
}

#if CROSSINK_APP_CAP_TOUCH
bool DictionaryDefinitionActivity::handleTouchDictionaryLookup() {
  if (!showLookupButton || !mappedInput.hasTouch() || RenderLock::peek()) return false;

  int touchX = 0;
  int touchY = 0;
  unsigned long heldMs = 0;
  if (!mappedInput.isScreenTouchTapCandidate(touchX, touchY, heldMs)) {
    touchDictionaryLookupHandled_ = false;
    return false;
  }
  if (touchDictionaryLookupHandled_ || heldMs < kTouchDefinitionLookupHoldMs) {
    return false;
  }
  touchDictionaryLookupHandled_ = true;

  if (!enterWordSelectMode()) return false;

  bool touchedWord = false;
  navigator.selectWordAtPoint(touchX, touchY, getLineHeight(), &touchedWord);
  if (!touchedWord || !navigator.beginTouchMultiSelect()) {
    isWordSelectMode = false;
    return false;
  }

  touchDragLookup_ = true;
  // Keep the word visibly selected before a release starts the background
  // lookup, matching the reader-page touch-and-hold flow.
  requestUpdateAndWait();
  return true;
}
#endif

void DictionaryDefinitionActivity::loop() {
  // Own the complete long-press gesture. Returning to the reader while Back is
  // still held would let the reader fire its configured long-press shortcut.
  if (handleLongPressExitAll(showLookupButton)) return;

#if CROSSINK_APP_CAP_TOUCH
  int modalTouchX = 0;
  int modalTouchY = 0;
  if (hasModalBackground() && mappedInput.hasTouch() && !controller.requiresBackgroundRedrawAfterOverlay() &&
      mappedInput.wasScreenTapped(modalTouchX, modalTouchY) && !modalContains(modalTouchX, modalTouchY)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
#endif

  // --- Controller active (LookingUp / AltFormPrompt / NotFound) ---
  if (controller.isActive()) {
#if CROSSINK_APP_CAP_TOUCH
    int failureTouchX = 0;
    int failureTouchY = 0;
    if (hasModalBackground() && controller.hasFailureFeedback() && showTouchDictionarySwitch() &&
        mappedInput.wasScreenTapped(failureTouchX, failureTouchY) &&
        dictionarySwitchButtonContains(failureTouchX, failureTouchY) &&
        controller.dismissFailureForDictionarySwitch()) {
      openDictionarySwitch();
      return;
    }
#endif
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        const bool wasBackNav = chainBackNavInProgress;
        const bool wasDictionarySwitch = dictionarySwitchLookupInProgress;
        const bool willLog = !wasBackNav && controller.getRecordHistory();
        {
          // A dictionary-picker return queues a background repaint while the
          // lookup worker can finish immediately. Serialize reflow with that
          // repaint: it temporarily unloads the dictionary font, and measuring
          // against the missing font otherwise collapses pagination to one line.
          RenderLock lock(*this);
          if (!wasBackNav && !wasDictionarySwitch) {
            // Forward: push a back-entry for the word being left (current headword,
            // on currentPage), referencing its history position.
            chain_.onForward(static_cast<uint16_t>(currentPage), willLog);
          }
          chainBackNavInProgress = false;
          dictionarySwitchLookupInProgress = false;
          headword = controller.getFoundWord();
          foundLocation = controller.getFoundLocation();
          wrapText();  // resets currentPage to 0 and loads page 0
          if (wasBackNav) {
            // Re-derive the now-current word's history position and restore its page.
            chain_.setCurrentHistIndex(pendingBack_.histIndex);
            currentPage = (pendingBack_.page < totalPages) ? pendingBack_.page : (totalPages - 1);
            if (currentPage < 0) currentPage = 0;
            if (currentPage > 0) loadPage(currentPage);
          }
          isWordSelectMode = false;
        }
        requestUpdate();
        // Chain-forward records; chain-back-nav does not.
        LookupHistory::addWordIf(cachePath, controller.getLookupWord(),
                                 DictionaryLookupController::toHistStatus(controller.getFoundStatus()), willLog);
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        dictionarySwitchLookupInProgress = false;
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        dictionarySwitchLookupInProgress = false;
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::SwitchDictionary:
        openDictionarySwitch();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        dictionarySwitchLookupInProgress = false;
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // --- Word-select mode ---
  if (isWordSelectMode) {
    if (navigator.handleNavigation(mappedInput, renderer)) {
      requestUpdate();
    }

#if CROSSINK_APP_CAP_TOUCH
    if (touchDragLookup_) {
      int dragX = 0;
      int dragY = 0;
      if (mappedInput.isScreenTouchHeld(dragX, dragY)) {
        if (navigator.selectWordAtPoint(dragX, dragY, getLineHeight())) {
          requestUpdate();
        }
        return;
      }

      touchDragLookup_ = false;
      controller.lookupOrPopup(navigator.finishTouchMultiSelect(), navigator.getLookupSelectionWordCount());
      return;
    }

    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTouchDown(touchX, touchY)) {
      bool touchedWord = false;
      navigator.selectWordAtPoint(touchX, touchY, getLineHeight(), &touchedWord);
      if (touchedWord && navigator.beginTouchMultiSelect()) {
        touchDragLookup_ = true;
        // Finish this fast refresh before lookup can replace the definition,
        // so the touched word always provides visible press feedback on e-ink.
        requestUpdateAndWait();
      }
      return;
    }
#endif

    if (controller.handleMultiSelect(navigator)) return;

    if (!navigator.isMultiSelecting()) {
      if (controller.handleConfirmLookup(navigator)) return;

      // Short press Back: exit word-select mode.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
      }
    }
    return;
  }

  // --- View mode ---
#if CROSSINK_APP_CAP_TOUCH
  int touchX = 0;
  int touchY = 0;
  if (showTouchDictionarySwitch() && hasClippingRequest_ && mappedInput.wasScreenTapped(touchX, touchY) &&
      dictionaryCreateClippingButtonContains(touchX, touchY)) {
    setResult(ActivityResult{clippingRequest_});
    finish();
    return;
  }
  if (showTouchDictionarySwitch() && mappedInput.wasScreenTapped(touchX, touchY) &&
      dictionarySwitchButtonContains(touchX, touchY)) {
    openDictionarySwitch();
    return;
  }
#endif

  if (showLookupButton && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openDictionarySwitch();
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (showLookupButton && mappedInput.hasTouch() && mappedInput.wasLeftEdgeGesture()) {
    setResult(ActivityResult{});
    finish();
    return;
  }
#endif

#if CROSSINK_APP_CAP_TOUCH
  if (handleTouchDictionaryLookup()) return;
#endif

  const auto swipe = mappedInput.wasSwipe();
  const bool prevPage =
      DictUtils::dictionaryPageButtonTriggered(mappedInput, true) || swipe == MappedInputManager::SwipeDir::Down;
  const bool nextPage =
      DictUtils::dictionaryPageButtonTriggered(mappedInput, false) || swipe == MappedInputManager::SwipeDir::Up;

  if (prevPage && currentPage > 0) {
    currentPage--;
    loadPage(currentPage);
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    loadPage(currentPage);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (showLookupButton) {
      if (enterWordSelectMode()) requestUpdate();
    } else {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      (!showLookupButton || mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS)) {
    if (!cachePath.empty() && !chain_.empty()) {
      pendingBack_ = chain_.pop();
      // Resolve the prior headword from the persisted history by distance-from-newest.
      const auto hist = LookupHistory::load(cachePath);  // newest-first
      if (pendingBack_.histIndex < hist.size()) {
        chainBackNavInProgress = true;
        controller.startLookup(hist[pendingBack_.histIndex].word, false);
        return;
      }
      // Unresolvable (should not happen under the depth cap) — fall through to exit.
    }
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::render(RenderLock&&) {
  if (hasModalBackground() && controller.consumeFullScreenChildDisturbance()) {
    modalBackgroundNeedsRedraw_ = true;
    modalCleanRefreshNeeded_ = true;
  }
  // Differential fast path: only when we're already in word-select mode AND
  // we set it up on the previous frame AND the controller has nothing pending.
  if (isWordSelectMode && nextRenderMode_ == RenderMode::Differential && !controller.isActive()) {
    const int currIdx = navigator.getCurrentFlatIndex();
    if (currIdx >= 0) {
      if (const auto* word = navigator.getWordAt(currIdx); word && renderer.isSdCardFont(word->fontId)) {
        if (auto* fcm = renderer.getFontCacheManager()) {
          const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(word->style) & 0x03));
          fcm->prewarmCache(word->fontId, navigator.getDisplay(*word), styleMask,
                            FontCacheManager::PreparationPolicy::DictionaryLean);
        }
      }
      const int lineHeight = getLineHeight();
      auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx, true);
      if (dirty.has_value()) {
        // Full panel push — matches DictionaryWordSelectActivity. Windowed refresh is not
        // wired up because the SDK's experimental path produces alternating black→white
        // failures on consecutive partial refreshes. Savings come from skipping page->render.
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        prevHighlightIdx_ = currIdx;
        return;
      }
      // fall through to full repaint path
    }
  }

  // Alt-form confirmation intentionally owns the full screen. Leave the reader
  // font unloaded while it is visible, then rebuild the background once after
  // the controller returns to the modal flow.
  if (hasModalBackground() && controller.requiresBackgroundRedrawAfterOverlay()) {
    controller.render();
    modalBackgroundNeedsRedraw_ = true;
    modalCleanRefreshNeeded_ = true;
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  // Full repaint path. The framebuffer retains the book pixels outside the
  // opaque, stable modal, so ordinary definition page turns only repaint the
  // modal. Rebuild the book after another screen or overlay disturbed it.
  if (hasModalBackground() && !isWordSelectMode && wordSelectHintsVisible_) {
    modalBackgroundNeedsRedraw_ = true;
  }
  if (hasModalBackground()) {
    redrawModalBackground();
  } else {
    renderer.clearScreen();
  }

  // After the picker closes, rebuild the reader once but do not redraw stale
  // definition glyphs. Keep lookup feedback inside the modal; the successful
  // replacement can then reuse this background without another font swap.
  if (dictionarySwitchLookupInProgress && controller.isLookingUp()) {
    if (hasModalBackground()) {
      drawModalFrame();
      const int messageY = modalY_ + (modalHeight_ - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, messageY, tr(STR_DICT_LOOKING_UP));
      displayModalBuffer();
    } else {
      GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
    }
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }
  const bool inlineFailureFeedback = hasModalBackground() && controller.hasFailureFeedback();
  if (!inlineFailureFeedback && controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    if (hasModalBackground() && controller.requiresBackgroundRedrawAfterOverlay()) {
      modalBackgroundNeedsRedraw_ = true;
    }
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  const auto metrics = UITheme::getInstance().getMetrics();
  const int bodyFontId = getDefinitionFontId();
  const int titleFontId = bodyFontId;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  const int indentStep = renderer.getTextWidth(bodyFontId, "   ");

  // Header chrome stays in the built-in UI font; the looked-up word uses the
  // active reader font and is drawn with the definition body after prewarming.
  Rect headerRect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                  metrics.headerHeight};
  if (hasModalBackground()) {
    drawModalFrame();
    const int innerPadding = metrics.optionPopupInnerPadding;
    headerRect = Rect{modalX_ + innerPadding, modalY_ + innerPadding, modalWidth_ - innerPadding * 2, titleLineHeight};
    if (metrics.optionPopupTitleSeparator) {
      const int separatorY = headerRect.y + headerRect.height + metrics.optionPopupTitleGap / 2;
      renderer.drawLine(headerRect.x, separatorY, headerRect.x + headerRect.width, separatorY, true);
    }
  } else {
    GUI.drawHeader(renderer, headerRect, "");
  }

  int titleRight = headerRect.x + headerRect.width;
  if (!isWordSelectMode && totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    const int pageInfoY =
        hasModalBackground()
            ? headerRect.y + (headerRect.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2
            : headerRect.y + headerRect.height - renderer.getLineHeight(SMALL_FONT_ID) - metrics.verticalSpacing / 2;
    const int pageInfoX = hasModalBackground() ? headerRect.x + headerRect.width - textWidth
                                               : renderer.getScreenWidth() - rightPadding - textWidth;
    renderer.drawText(SMALL_FONT_ID, pageInfoX, pageInfoY, pageInfo);
    titleRight = pageInfoX - metrics.optionPopupInnerPadding;
  }

  const int titleX = hasModalBackground() ? headerRect.x : headerRect.x + metrics.contentSidePadding;
  const int titleY = hasModalBackground() ? headerRect.y : headerRect.y + (headerRect.height - titleLineHeight) / 2;
  const int titleMaxWidth = std::max(0, titleRight - titleX);
  const std::string visibleHeadword =
      renderer.truncatedText(titleFontId, headword.c_str(), titleMaxWidth, EpdFontFamily::BOLD);
  auto renderTitle = [&]() {
    renderer.drawText(titleFontId, titleX, titleY, visibleHeadword.c_str(), true, EpdFontFamily::BOLD);
  };

  // Body: draw layout lines for the current page (BW pass). layoutLines holds
  // only the current page (Stage 2a streaming), so it is indexed from 0.
  const int lineHeight = getLineHeight();  // cached for loop + renderHighlight
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
      const PooledLine& line = layoutLines[i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(bodyFontId, x, y, kBullet);
        x += renderer.getTextWidth(bodyFontId, kBullet);
      }

      for (uint16_t segmentIdx = 0; segmentIdx < line.segmentCount; ++segmentIdx) {
        const PooledSegment& seg = layoutSegments[line.firstSegment + segmentIdx];
        const int segFontId = getDefinitionFontId(seg.isIpa);
        const char* segText = pagePool_.data() + seg.offset;
        renderer.drawText(segFontId, x, y, segText, true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, segText, seg.style);
          const int underlineY = y + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, segText, seg.style);
      }
    }
  };

  // SD fonts retain bitmaps for only the most recently prepared page. The
  // reader background above may have replaced that cache, so scan the visible
  // title and definition page before drawing them. The .dict reader was closed
  // by loadPage(), keeping font and dictionary SD access serialized.
  bool definitionTextRendered = false;
  if (renderer.isSdCardFont(bodyFontId)) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      // Keep this alive through the word-select overlay below. Its destructor
      // clears SD glyph bitmaps, and that overlay redraws the selected word.
      std::optional<FontCacheManager::PrewarmScope> definitionRenderScope;
      const auto prewarmAndRender = [&]() {
        definitionRenderScope.emplace(*fcm, FontCacheManager::PreparationPolicy::DictionaryLean);
        renderTitle();
        renderBody();  // scan pass
        if (!definitionRenderScope->endScanAndPrewarm()) {
          definitionRenderScope.reset();
          return false;
        }
        renderTitle();
        renderBody();  // prepared render
        return true;
      };

      definitionTextRendered = prewarmAndRender();
      if (!definitionTextRendered) {
        // A switched definition can require a different glyph/style set.
        // Release optional bitmap/kern caches, retain the advance table used by
        // layout, and retry once with the exact visible page glyphs.
        LOG_ERR("DICT", "SD-font definition prewarm failed; releasing caches and retrying");
        renderer.releaseSdCardFontForLowMemory(bodyFontId, /*preserveAdvanceTable=*/true);
        definitionTextRendered = prewarmAndRender();
        if (!definitionTextRendered) {
          useBuiltInDefinitionFontFallback();
          requestUpdate();
          return;
        }
      }
    }
  }
  if (!definitionTextRendered) {
    renderTitle();
    renderBody();
  }

  const char* inlineStatusMessage = nullptr;
  if (hasModalBackground() && controller.isLookingUp()) {
    inlineStatusMessage = tr(STR_DICT_LOOKING_UP);
  } else if (inlineFailureFeedback) {
    inlineStatusMessage = controller.getFailureMessage();
  } else if (hasModalBackground() && definitionReadFailed_) {
    inlineStatusMessage = tr(STR_DICT_READ_FAILED);
  }
  if (inlineStatusMessage) {
    const int messageHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.optionPopupInnerPadding * 2;
    const int messageY = modalY_ + (modalHeight_ - messageHeight) / 2;
    renderer.fillRect(modalX_ + metrics.optionPopupInnerPadding, messageY,
                      modalWidth_ - metrics.optionPopupInnerPadding * 2, messageHeight, false);
    renderer.drawCenteredText(UI_10_FONT_ID, messageY + metrics.optionPopupInnerPadding, inlineStatusMessage);
  }

  if (!hasModalBackground() && definitionReadFailed_) GUI.drawPopup(renderer, tr(STR_DICT_READ_FAILED));

  if (hasModalBackground() && !dictionaryName_.empty()) {
    const int innerPadding = metrics.optionPopupInnerPadding;
    const int footerLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
#if CROSSINK_APP_CAP_TOUCH
    const int switchButtonHeight =
        showTouchDictionarySwitch() ? kDictionarySwitchTouchHeight * (hasClippingRequest_ ? 2 : 1) : 0;
#else
    constexpr int switchButtonHeight = 0;
#endif
    const int footerY = modalY_ + modalHeight_ - switchButtonHeight - innerPadding - footerLineHeight;
    const int separatorY = footerY - metrics.optionPopupTitleGap / 2;
    const int footerBottom = modalY_ + modalHeight_ - switchButtonHeight;
    const int dictionaryNameY = separatorY + (footerBottom - separatorY - footerLineHeight) / 2;
    renderer.drawLine(modalX_, separatorY, modalX_ + modalWidth_, separatorY, true);
    const auto visibleName =
        renderer.truncatedText(UI_10_FONT_ID, dictionaryName_.c_str(), modalWidth_ - innerPadding * 2);
    renderer.drawText(UI_10_FONT_ID, modalX_ + innerPadding, dictionaryNameY, visibleName.c_str());
  }

#if CROSSINK_APP_CAP_TOUCH
  if (!isWordSelectMode && showTouchDictionarySwitch()) {
    const Rect buttonRect{modalX_,
                          modalY_ + modalHeight_ - kDictionarySwitchTouchHeight * (hasClippingRequest_ ? 2 : 1),
                          modalWidth_, kDictionarySwitchTouchHeight};
    renderer.drawLine(buttonRect.x, buttonRect.y, buttonRect.x + buttonRect.width, buttonRect.y, true);
    const char* label = tr(STR_SWITCH_DICTIONARY);
    const int labelX =
        buttonRect.x + (buttonRect.width - renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD)) / 2;
    const int labelY = buttonRect.y + (buttonRect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, labelX, labelY, label, true, EpdFontFamily::BOLD);
    if (hasClippingRequest_) {
      const Rect clippingRect{modalX_, modalY_ + modalHeight_ - kDictionarySwitchTouchHeight, modalWidth_,
                              kDictionarySwitchTouchHeight};
      renderer.drawLine(clippingRect.x, clippingRect.y, clippingRect.x + clippingRect.width, clippingRect.y, true);
      const char* clippingLabel = tr(STR_SAVE_CLIPPING);
      const int clippingLabelX =
          clippingRect.x +
          (clippingRect.width - renderer.getTextWidth(UI_10_FONT_ID, clippingLabel, EpdFontFamily::BOLD)) / 2;
      const int clippingLabelY = clippingRect.y + (clippingRect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, clippingLabelX, clippingLabelY, clippingLabel, true, EpdFontFamily::BOLD);
    }
  }
#endif

  // Word-select mode: overlay highlighted word(s) and prime snapshot for next frame.
  // The -1 prevWordIdx literal is load-bearing: renderHighlightDifferential uses
  // prevWordIdx < 0 as the signal "framebuffer was just redrawn from scratch,
  // discard any stale snapshot rather than restoring it on top of fresh pixels."
  // This is the only path that disturbs the framebuffer outside the differential
  // cycle, so it's also the only call site that must pass -1.
  if (isWordSelectMode) {
    const int currIdx = navigator.getCurrentFlatIndex();
    bool snapshotPrimed = false;
    if (currIdx >= 0) {
      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx, true);
      snapshotPrimed = setup.has_value();
    }
    if (!snapshotPrimed) {
      navigator.renderHighlight(renderer, lineHeight, true);
    }

    DictUtils::drawWordSelectButtonHints(renderer, mappedInput, navigator);
    wordSelectHintsVisible_ = true;
    displayModalBuffer();

    prevHighlightIdx_ = currIdx;
    nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
    return;
  }

  // View mode: differential state is irrelevant — reset so that the next entry
  // into word-select starts cleanly with a full repaint.
  nextRenderMode_ = RenderMode::FullPage;
  prevHighlightIdx_ = -1;

  // Button hints
  const char* btn2 = inlineFailureFeedback ? tr(STR_DONE) : (showLookupButton ? tr(STR_LOOKUP_SHORT) : "");
  const char* btn3 = showLookupButton ? tr(STR_DICT_SWITCH) : "";
  const char* btn4 = nullptr;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  wordSelectHintsVisible_ = false;

  if (hasModalBackground()) {
    displayModalBuffer();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  // Skip the full-screen grayscale anti-aliasing overlay here. This modal is
  // short-lived, and the 48KB BW backup it needs can fragment the heap enough
  // that the reader's largest free block does not recover after returning.
}
