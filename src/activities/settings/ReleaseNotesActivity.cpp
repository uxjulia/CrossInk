#include "ReleaseNotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AssetDownloadActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MAX_LINES_PER_PARAGRAPH = 30;
constexpr int PATH_BOX_PADDING = 12;  // Left/right space between the menu path box and its text
constexpr int PATH_BOX_VPAD = 10;     // Top/bottom space inside the menu path box
constexpr int PATH_BOX_MARGIN = 8;    // Space between the box and the text around it
constexpr char PATH_SEPARATOR[] = " > ";
}  // namespace

ReleaseNotesActivity::ReleaseNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Mode mode)
    : Activity("ReleaseNotes", renderer, mappedInput), mode(mode) {}

bool ReleaseNotesActivity::usesBackHeader() const {
  return mode == Mode::FromSettings && mappedInput.hasTouchHardware();
}

Rect ReleaseNotesActivity::headerRect() const {
  if (usesBackHeader()) return TouchHeaderBackButton::headerRect(renderer, mappedInput);
  // Keep the header beside the button hints, which sit on a side edge in landscape.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch(), false);
  return UITheme::getHeaderRect(renderer, UITheme::getInstance().getMetrics().headerHeight, safe);
}

const char* ReleaseNotesActivity::primaryLabel() const { return primaryText; }

int ReleaseNotesActivity::actionCount() const {
  // Touch FirstRun adds Later; button devices close with Back, so only the main action is on screen.
  return mappedInput.hasTouch() && mode == Mode::FirstRun ? 2 : 1;
}

namespace {
// On-screen action buttons sit at the bottom of the content area, above the button hints when present.
TouchActionButtons::Layout actionLayout(const GfxRenderer& renderer, const bool hasButtonHints, const int count) {
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect screen = theme.getScreenSafeArea(renderer, hasButtonHints, false);
  const int totalHeight =
      TouchActionButtons::kDefaultHeight * count + TouchActionButtons::kDefaultGap * std::max(0, count - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, static_cast<uint8_t>(count));
}
}  // namespace

#ifdef SIMULATOR
Rect ReleaseNotesActivity::simulatorActionRect(const int index) const {
  const auto actions = actionLayout(renderer, !mappedInput.hasTouch(), actionCount());
  return index >= 0 && index < actions.count ? actions.buttons[index] : Rect();
}
#endif

void ReleaseNotesActivity::onEnter() {
  Activity::onEnter();
  if (mode == Mode::FirstRun) activityManager.markReleaseNotesShown();

  // Use the saved preference, not the active code: after upgrading from firmware with built-in
  // translations, a saved non-English language with no installed pack runs as English.
  const char* code = SETTINGS.languageCode;
  updateOffered = releaseNotesOfferLanguageUpdate(code, I18N.getCode(), APP_STATE.releaseNotesUpdateTried);
  languageIndex = static_cast<int>(I18n::languageFromCode(code));
  page = 0;
  layout();
  requestUpdate();
}

void ReleaseNotesActivity::onExit() {
  markSeen();
  Activity::onExit();
}

void ReleaseNotesActivity::markSeen() {
  if (mode != Mode::FirstRun || leavingForUpdate || seenMarked) return;
  seenMarked = true;
  if (APP_STATE.releaseNotesSeen >= RELEASE_NOTES_REVISION) return;
  APP_STATE.releaseNotesSeen = RELEASE_NOTES_REVISION;
  if (!APP_STATE.saveToFile()) LOG_ERR("NOTES", "Could not save release notes state");
}

void ReleaseNotesActivity::addGap(const int height) {
  if (height > 0) lines.push_back({std::string(), LineKind::Gap, static_cast<uint16_t>(height), false});
}

void ReleaseNotesActivity::addWrapped(const char* text, const LineKind kind, const int fontId, const int width,
                                      const EpdFontFamily::Style style) {
  if (!text) return;
  const uint16_t height = static_cast<uint16_t>(renderer.getLineHeight(fontId));
  const char* segment = text;
  while (true) {
    const char* end = std::strchr(segment, '\n');
    const std::string paragraph = end ? std::string(segment, end - segment) : std::string(segment);
    if (paragraph.empty()) {
      lines.push_back({std::string(), kind, height, false});
    } else {
      // Move wrapped strings into layout without copying their text.
      for (auto& wrapped : renderer.wrappedText(fontId, paragraph.c_str(), width, MAX_LINES_PER_PARAGRAPH, style)) {
        // cppcheck-suppress useStlAlgorithm
        lines.push_back({std::move(wrapped), kind, height, false});
      }
    }
    if (!end) break;
    segment = end + 1;
  }
}

void ReleaseNotesActivity::addItem(const char* title, const char* body, const StrId* path, const uint8_t pathLength,
                                   const char* literal, const int width) {
  const size_t first = lines.size();
  addWrapped(title, LineKind::Title, UI_10_FONT_ID, width, EpdFontFamily::BOLD);
  if (first < lines.size()) lines[first].startsItem = true;
  addWrapped(body, LineKind::Body, UI_10_FONT_ID, width, EpdFontFamily::REGULAR);
  if (path && pathLength > 0) {
    std::string joined;
    for (uint8_t i = 0; i < pathLength; ++i) {
      if (i > 0) joined += PATH_SEPARATOR;
      joined += I18N.get(path[i]);
    }
    addGap(PATH_BOX_MARGIN + PATH_BOX_VPAD);
    addWrapped(joined.c_str(), LineKind::Path, UI_10_FONT_ID, width - PATH_BOX_PADDING * 2, EpdFontFamily::REGULAR);
    addGap(PATH_BOX_VPAD + PATH_BOX_MARGIN);
  }
  addWrapped(literal, LineKind::Body, UI_10_FONT_ID, width, EpdFontFamily::REGULAR);
  addGap(renderer.getLineHeight(UI_10_FONT_ID) / 2);
}

// Builds every wrapped line and the page breaks once per enter or orientation change;
// render() only draws the slice for the current page.
void ReleaseNotesActivity::layout() {
  lines.clear();
  pageStarts.clear();
  layoutWidth = renderer.getScreenWidth();
  layoutHeight = renderer.getScreenHeight();
  layoutTouch = mappedInput.hasTouch();
  layoutOrientation = renderer.getOrientation();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // Button hints sit on the bottom edge in portrait but on a side edge in landscape.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch(), false);
  contentX = safe.x + metrics.contentSidePadding;
  contentWidth = std::max(1, safe.width - 2 * metrics.contentSidePadding);
  const int width = contentWidth;

  if (updateOffered) {
    snprintf(primaryText, sizeof(primaryText), tr(STR_UPDATE_LANGUAGE_FORMAT),
             I18N.getLanguageName(static_cast<Language>(languageIndex)));
  } else {
    snprintf(primaryText, sizeof(primaryText), "%s", tr(STR_INSTALL_LANGUAGES));
  }

  const Rect header = headerRect();
  bodyTop = header.y + header.height + metrics.verticalSpacing;
  bodyBottom = actionLayout(renderer, !mappedInput.hasTouch(), actionCount()).container.y;
  bodyBottom -= lineHeight + metrics.verticalSpacing;  // Reserved row for the page indicator, clear of the buttons.

  lines.reserve(48);
  if (updateOffered) addItem(primaryText, tr(STR_LANGUAGE_PACK_OUTDATED), nullptr, 0, nullptr, width);
  for (size_t i = 0; i < RELEASE_NOTES_COUNT; ++i) {
    const ReleaseNoteItem& item = RELEASE_NOTES[i];
    addItem(I18N.get(item.title), item.body == StrId::_COUNT ? nullptr : I18N.get(item.body), item.path,
            item.pathLength, item.literal, width);
  }

  const int available = bodyBottom - bodyTop;
  const size_t count = lines.size();
  size_t i = 0;
  while (i < count) {
    while (i < count && lines[i].kind == LineKind::Gap) ++i;  // Pages never start with spacing.
    if (i >= count) break;
    const size_t start = i;
    int used = 0;
    while (i < count && used + lines[i].height <= available) used += lines[i++].height;
    if (i == start) ++i;  // A line taller than the page still gets its own page.
    size_t afterGaps = i;
    while (afterGaps < count && lines[afterGaps].kind == LineKind::Gap) ++afterGaps;
    if (afterGaps > i && (afterGaps >= count || lines[afterGaps].startsItem)) {
      i = afterGaps;  // Only an item's trailing spacing overflowed; the item itself fits.
    } else if (i < count && !lines[i].startsItem) {
      // Mid-item break: move the whole item to the next page when it fits on one page.
      size_t itemStart = i - 1;
      while (itemStart > start && !lines[itemStart].startsItem) --itemStart;
      int itemHeight = 0;
      for (size_t k = itemStart; k < count && (k == itemStart || !lines[k].startsItem); ++k)
        itemHeight += lines[k].height;
      if (itemStart > start && lines[itemStart].startsItem && itemHeight <= available) {
        i = itemStart;
      } else if (i - 1 > start && lines[i - 1].kind == LineKind::Title) {
        --i;  // At least keep a title with its body.
      }
    }
    pageStarts.push_back(static_cast<uint16_t>(start));
  }
  if (pageStarts.empty()) pageStarts.push_back(0);
  page = std::min(page, static_cast<int>(pageStarts.size()) - 1);
}

void ReleaseNotesActivity::nextPage() {
  if (page + 1 >= static_cast<int>(pageStarts.size())) return;
  ++page;
  requestUpdate();
}

void ReleaseNotesActivity::previousPage() {
  if (page <= 0) return;
  --page;
  requestUpdate();
}

void ReleaseNotesActivity::close() { finish(); }

void ReleaseNotesActivity::onPrimaryAction() {
  if (updateOffered) {
    startUpdate();
    return;
  }
  if (mode == Mode::FromSettings) {
    MenuResult result;
    result.action = RESULT_OPEN_LANGUAGE_DOWNLOADS;
    setResult(std::move(result));
    finish();
    return;
  }
  markSeen();
  activityManager.goToSettings(false, true);
}

// Mirrors SettingsActivity::startLanguageDownload(): persist, then reboot into the network downloader.
void ReleaseNotesActivity::startUpdate() {
  const uint16_t previousTried = APP_STATE.releaseNotesUpdateTried;
  APP_STATE.releaseNotesUpdateTried = RELEASE_NOTES_REVISION;
  APP_STATE.pendingOverlayResume.clear();
  if (APP_STATE.saveToFile() && SETTINGS.saveToFile()) {
    RenderLock restartLock(*this);
    leavingForUpdate = true;  // releaseNotesSeen stays unset so the translated notes show after the download.
    silentRestartToNetwork(
        NetworkBootTarget::DOWNLOAD_ASSETS,
        AssetDownloadActivity::bootPayload(AssetCatalog::Kind::Translation, static_cast<uint8_t>(languageIndex)));
  } else {
    LOG_ERR("NOTES", "Cannot save state before network restart");
    APP_STATE.releaseNotesUpdateTried = previousTried;
    errorText = StrId::STR_LANGUAGE_SAVE_FAILED;
    requestUpdate();
  }
}

void ReleaseNotesActivity::loop() {
  if (renderer.getScreenWidth() != layoutWidth || renderer.getScreenHeight() != layoutHeight ||
      mappedInput.hasTouch() != layoutTouch || renderer.getOrientation() != layoutOrientation) {
    {
      RenderLock lock(*this);
      layout();
    }
    requestUpdate();
  }

  if (usesBackHeader() && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    close();
    return;
  }

  int x = 0;
  int y = 0;
  // Act on a completed tap, so a swipe that starts on the buttons pages instead of dismissing.
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(x, y)) {
    const int action = TouchActionButtons::indexAt(actionLayout(renderer, false, actionCount()), x, y,
                                                   TouchActionButtons::kDefaultGap / 2);
    if (action == 0) {
      onPrimaryAction();
      return;
    }
    if (action == 1) {
      close();
      return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    nextPage();
  } else if (swipe == MappedInputManager::SwipeDir::Down) {
    previousPage();
  }

  buttonNavigator.onNextRelease([this] { nextPage(); });
  buttonNavigator.onPreviousRelease([this] { previousPage(); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onPrimaryAction();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
  }
}

void ReleaseNotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int x = contentX;

  char version[16];
  snprintf(version, sizeof(version), "v%s", RELEASE_NOTES_VERSION);
  const char* title = mode == Mode::FirstRun ? tr(STR_WHATS_NEW) : tr(STR_RELEASE_NOTES);
  const Rect header = headerRect();
  if (usesBackHeader()) {
    TouchHeaderBackButton::draw(renderer, header, title, false, 0, version);
  } else {
    GUI.drawHeader(renderer, header, title, version);
  }

  const size_t first = pageStarts[page];
  const size_t last = page + 1 < static_cast<int>(pageStarts.size()) ? pageStarts[page + 1] : lines.size();
  int y = bodyTop;
  int boxTop = -1;
  int boxBottom = -1;
  const auto closeBox = [&] {
    if (boxTop >= 0) renderer.drawRect(x, boxTop, contentWidth, boxBottom + PATH_BOX_VPAD - boxTop);
    boxTop = -1;
  };
  for (size_t i = first; i < last; ++i) {
    const Line& line = lines[i];
    if (line.kind != LineKind::Path) closeBox();
    switch (line.kind) {
      case LineKind::Title:
        renderer.drawText(UI_10_FONT_ID, x, y, line.text.c_str(), true, EpdFontFamily::BOLD);
        break;
      case LineKind::Body:
        renderer.drawText(UI_10_FONT_ID, x, y, line.text.c_str());
        break;
      case LineKind::Path:
        if (boxTop < 0) boxTop = y - PATH_BOX_VPAD;
        boxBottom = y + line.height;
        renderer.drawText(UI_10_FONT_ID, x + PATH_BOX_PADDING, y, line.text.c_str());
        break;
      case LineKind::Gap:
        break;
    }
    y += line.height;
  }
  closeBox();

  const int footerY = bodyBottom;
  if (errorText != StrId::_COUNT) {
    renderer.drawText(UI_10_FONT_ID, x, footerY, I18N.get(errorText), true, EpdFontFamily::BOLD);
  } else if (pageStarts.size() > 1) {
    char indicator[32];
    snprintf(indicator, sizeof(indicator), tr(STR_SYNC_PAGE_TOTAL_FORMAT), page + 1,
             static_cast<int>(pageStarts.size()));
    const int width = renderer.getTextWidth(UI_10_FONT_ID, indicator);
    renderer.drawText(UI_10_FONT_ID, x + contentWidth - width, footerY, indicator);
  }

  const char* labels[2] = {primaryLabel(), tr(STR_LATER)};
  if (mappedInput.hasTouch()) {
    TouchActionButtons::draw(renderer, actionLayout(renderer, false, actionCount()), labels, 0, -1, UI_10_FONT_ID);
  } else {
    // The on-screen action is the only focusable element, so it is always selected and Confirm presses it.
    TouchActionButtons::draw(renderer, actionLayout(renderer, true, actionCount()), labels, 0, 0, UI_10_FONT_ID);
    const auto hints =
        mappedInput.mapLabels(mappedInput.withBackArrow(mode == Mode::FirstRun ? tr(STR_DONE) : tr(STR_BACK)),
                              tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  }

  renderer.displayBuffer();
}
