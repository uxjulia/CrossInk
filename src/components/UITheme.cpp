#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "DeviceCapabilities.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

namespace {
constexpr char kWidthPlaceholder[] = "[WIDTH]";
constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kWidthPlaceholderLength = sizeof(kWidthPlaceholder) - 1;
constexpr size_t kHeightPlaceholderLength = sizeof(kHeightPlaceholder) - 1;

int displayStatusBarHeightIncrease(const uint8_t textSize) {
  // Built-in Inter advances are 20/25/30px. Enlarged lanes add the same 4px
  // padding as reader lanes, compared with the existing 19px Small lane.
  // Keep metric consumers independent of renderer initialization and reader size.
  switch (textSize) {
    case 1:
      return 25 + 4 - 19;
    case 2:
      return 30 + 4 - 19;
    default:
      return 0;
  }
}

int drawCenteredTextLines(const GfxRenderer& renderer, const Rect screen, const int fontId, int y,
                          const std::vector<std::string>& lines, const bool black, const EpdFontFamily::Style style,
                          const int lineSpacing) {
  if (lines.empty()) return 0;

  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    UITheme::drawCenteredText(renderer, screen, fontId, y, line.c_str(), black, style);
    y += lineHeight + lineSpacing;
  }
  return lineHeight * static_cast<int>(lines.size()) + lineSpacing * (static_cast<int>(lines.size()) - 1);
}

std::string addBmpSuffix(const std::string& path, const char* suffix) {
  const size_t extPos = path.rfind(".bmp");
  if (extPos == std::string::npos) {
    return path + suffix;
  }
  std::string suffixedPath = path;
  suffixedPath.insert(extPos, suffix);
  return suffixedPath;
}
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() : currentMetrics(&LyraMetrics::values), currentTheme(std::make_unique<LyraTheme>()) {
  // Static construction must not log or depend on cross-TU serial initialization;
  // main.cpp reloads the saved theme after setup.
  rebuildMetricVariants();
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

bool UITheme::supportsCoverGrid() {
#if defined(SIMULATOR)
  return
#if defined(SIMULATOR_DEVICE_STICKY) || defined(SIMULATOR_DEVICE_X4_PRO) || defined(SIMULATOR_DEVICE_X4_CLASSIC)
      true;
#else
      false;
#endif
#else
  return psramHeapAvailable();
#endif
}

bool UITheme::hasCoverGridHome() {
  return SETTINGS.uiTheme == CrossPointSettings::UI_THEME::COVER_GRID && supportsCoverGrid();
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  if (type == CrossPointSettings::UI_THEME::COVER_GRID && !supportsCoverGrid()) {
    type = CrossPointSettings::UI_THEME::LYRA;
  }
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::COVER_GRID:
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
      LOG_DBG("UI", "Using Lyra Carousel theme");
      currentTheme = std::make_unique<LyraCarouselTheme>();
      currentMetrics = &LyraCarouselMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::MINIMAL:
      LOG_DBG("UI", "Using Minimal theme");
      currentTheme = std::make_unique<MinimalTheme>();
      currentMetrics = &MinimalMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::DASHBOARD:
      LOG_DBG("UI", "Using Dashboard theme");
      currentTheme = std::make_unique<DashboardTheme>();
      currentMetrics = &DashboardMetrics::values;
      break;
    default:
      LOG_ERR("UI", "Unknown theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
  }
  rebuildMetricVariants();
}

void UITheme::rebuildMetricVariants() {
  for (size_t index = 0; index < metricVariants.size(); ++index) {
    auto& metrics = metricVariants[index];
    metrics = *currentMetrics;
    if (index >= 3) metrics.buttonHintsHeight = 0;
    const int increase = displayStatusBarHeightIncrease(index % 3);
    metrics.batteryBarHeight += increase;
    metrics.headerHeight += increase;
    metrics.homeTopPadding += increase;
  }
}

const ThemeMetrics& UITheme::getMetrics() const {
  const uint8_t setting = SETTINGS.displayStatusBarTextSize;
  const size_t size = setting < 3 ? setting : 0;
#if CROSSINK_APP_CAP_TOUCH
  // Touch initializes after static construction on some profiles. Both sets
  // already exist, so this lookup never mutates a published metric object.
  return metricVariants[size + (gpio.hasTouch() ? 3 : 0)];
#else
  return metricVariants[size];
#endif
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const auto safe = getInstance().getScreenSafeArea(renderer, hasButtonHints, false);
  int contentTop = std::max(safe.y, static_cast<int>(metrics.topPadding));
  if (hasHeader) {
    const auto header = getHeaderRect(renderer, metrics.headerHeight, safe);
    contentTop = header.y + header.height + metrics.verticalSpacing;
  }
  if (hasTabBar) contentTop += metrics.tabBarHeight;
  const int availableHeight = std::max(0, safe.y + safe.height - contentTop - extraReservedHeight);
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const int hintReserve = getButtonHintsReserve(renderer);
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= hintReserve;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += hintReserve;
        safeArea.width -= hintReserve;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += hintReserve;
        safeArea.height -= hintReserve;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= hintReserve;
      }
      break;
  }
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  // Touch capability is fixed per build; the shared layout covers both targets.
  // cppcheck-suppress knownConditionTrueFalse
  if (hasSideButtonHints && !gpio.hasTouch()) {
    if (deviceHasEdgeSideButtons(gpio)) left += getMetrics().sideButtonHintsWidth;
    right += getMetrics().sideButtonHintsWidth;
  }
  const int x = std::max(safeArea.x, left);
  const int y = std::max(safeArea.y, top);
  const int endX = std::min(safeArea.x + safeArea.width, screenWidth - right);
  const int endY = std::min(safeArea.y + safeArea.height, screenHeight - bottom);
  safeArea = Rect{x, y, std::max(0, endX - x), std::max(0, endY - y)};
  return safeArea;
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int coverHeight) {
  if (coverHeight <= 0) {
    return "";
  }
  // Use int64_t so large heights cannot overflow before division.
  const int coverWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 2 + 1) / 3);
  return getCoverThumbPath(coverBmpPath, coverWidth, coverHeight);
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int width, int height,
                                       bool allowLegacyFallback) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  const size_t initialWidthPos = coverBmpPath.find(kWidthPlaceholder, 0);
  const size_t initialHeightPos = coverBmpPath.find(kHeightPlaceholder, 0);
  const bool hasWidthPlaceholder = initialWidthPos != std::string::npos;
  const bool hasHeightPlaceholder = initialHeightPos != std::string::npos;

  if (!hasWidthPlaceholder && !hasHeightPlaceholder) {
    return coverBmpPath;
  }
  if ((hasWidthPlaceholder &&
       coverBmpPath.find(kWidthPlaceholder, initialWidthPos + kWidthPlaceholderLength) != std::string::npos) ||
      (hasHeightPlaceholder &&
       coverBmpPath.find(kHeightPlaceholder, initialHeightPos + kHeightPlaceholderLength) != std::string::npos)) {
    return "";
  }
  if (!hasHeightPlaceholder) {
    return "";
  }

  std::string thumbPath = coverBmpPath;
  size_t widthPos = thumbPath.find(kWidthPlaceholder, 0);
  if (widthPos != std::string::npos) {
    thumbPath.replace(widthPos, kWidthPlaceholderLength, std::to_string(width));
  }
  size_t pos = thumbPath.find(kHeightPlaceholder, 0);
  if (pos != std::string::npos) {
    if (hasWidthPlaceholder) {
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
    } else {
      std::string legacyPath = thumbPath;
      legacyPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(width) + "x" + std::to_string(height));
      if (allowLegacyFallback && !Storage.exists(thumbPath.c_str()) && Storage.exists(legacyPath.c_str())) {
        return legacyPath;
      }
    }
  }
  return thumbPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getDisplayStatusBarFontId() {
  switch (SETTINGS.displayStatusBarTextSize) {
    case 1:
      return UI_10_FONT_ID;
    case 2:
      return UI_12_FONT_ID;
    default:
      return SMALL_FONT_ID;
  }
}

int UITheme::getDisplayStatusBarHeightIncrease() {
  return displayStatusBarHeightIncrease(SETTINGS.displayStatusBarTextSize);
}

int UITheme::getDisplayStatusBarTextHeight(const GfxRenderer& renderer) {
  const int baseline = getInstance().getMetrics().statusBarVerticalMargin;
  const int fontId = getDisplayStatusBarFontId();
  return fontId == SMALL_FONT_ID ? baseline : std::max(baseline, renderer.getLineHeight(fontId) + 4);
}

int UITheme::getReaderStatusBarFontId() {
  switch (SETTINGS.statusBarTextSize) {
    case 1:
      return UI_10_FONT_ID;
    case 2:
      return UI_12_FONT_ID;
    default:
      return SMALL_FONT_ID;
  }
}

int UITheme::getReaderStatusBarTextHeight(const GfxRenderer& renderer) {
  const int defaultHeight = getInstance().getMetrics().statusBarVerticalMargin;
  const int fontId = getReaderStatusBarFontId();
  if (fontId == SMALL_FONT_ID) return defaultHeight;
  // Include ascenders and descenders, with room to center the line in the bar.
  constexpr int textPadding = 4;
  return std::max(defaultHeight, renderer.getLineHeight(fontId) + textPadding);
}

int UITheme::getStatusBarHeight(const GfxRenderer& renderer) {
  return getReaderStatusBarHeight(ReaderStatusBarPosition::Bottom, renderer);
}

int UITheme::getReaderStatusBarHeight(const ReaderStatusBarPosition position, const GfxRenderer& renderer,
                                      const ReaderStatusBarConfig* overrideConfig) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const auto config = overrideConfig ? *overrideConfig : SETTINGS.readerStatusBar(position);
  if (config.hidden) return 0;
  const bool hasText = config.hasTextItems(halClock.isAvailable());
  const int progressSpace = config.progressBar != CrossPointSettings::HIDE_PROGRESS
                                ? static_cast<int>((config.progressBarThickness + 1) * 2) + metrics.progressBarMarginTop
                                : 0;
  return readerStatusBarTotalHeight(position, hasText, progressSpace, getReaderStatusBarTextHeight(renderer));
}

int UITheme::getProgressBarHeight() { return getReaderProgressBarHeight(ReaderStatusBarPosition::Bottom); }

int UITheme::getReaderProgressBarHeight(const ReaderStatusBarPosition position) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const auto config = SETTINGS.readerStatusBar(position);
  return !config.hidden && config.progressBar != CrossPointSettings::HIDE_PROGRESS
             ? static_cast<int>((config.progressBarThickness + 1) * 2) + metrics.progressBarMarginTop
             : 0;
}

int UITheme::getButtonHintsBottomInset(const GfxRenderer& renderer) { return renderer.getViewableInsets().edges[2]; }

int UITheme::getButtonHintsReserve(const GfxRenderer& renderer) {
  const int height = getInstance().getMetrics().buttonHintsHeight;
  return height > 0 ? height + getButtonHintsBottomInset(renderer) : 0;
}

int UITheme::getHintSafeX(const GfxRenderer& renderer, const int x, const int width) {
  const auto edges = renderer.getViewableInsets().rotated(static_cast<unsigned>(renderer.getOrientation())).edges;
  return std::clamp(x, static_cast<int>(edges[3]),
                    std::max(static_cast<int>(edges[3]), renderer.getScreenWidth() - edges[1] - width));
}

int UITheme::getTopStatusBarY(const GfxRenderer& renderer) {
  const int legacyY = getInstance().getMetrics().topPadding + getTopStatusBarInset(renderer);
  return renderer.getViewableInsets().topOrigin(static_cast<unsigned>(renderer.getOrientation()), legacyY);
}

Rect UITheme::getHeaderRect(const GfxRenderer& renderer, const int height) {
  const auto insets = renderer.getViewableInsets().rotated(static_cast<unsigned>(renderer.getOrientation()));
  // drawDisplayStatusBar adds the existing board offset exactly once.
  const int y =
      std::max(static_cast<int>(insets.edges[0]), getTopStatusBarY(renderer) - getTopStatusBarInset(renderer));
  return Rect{insets.edges[3], y, renderer.getScreenWidth() - insets.edges[1] - insets.edges[3], height};
}

Rect UITheme::getHeaderRect(const GfxRenderer& renderer, const int height, const Rect& area) {
  auto header = getHeaderRect(renderer, height);
  const int right = std::min(header.x + header.width, area.x + area.width);
  header.x = std::max(header.x, area.x);
  header.width = std::max(0, right - header.x);
  header.y = std::max(header.y, area.y);
  return header;
}

int UITheme::getTopStatusBarInset(const GfxRenderer& renderer) {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  // The Sticky panel remains usable closer to its top edge than the shared
  // status-bar layout assumes. Keep the clock and battery in that space.
  (void)renderer;
  return -5;
#elif (defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO) || \
    (defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC)
  // The X4 Pro and X4 Classic panels sit slightly recessed behind the
  // portrait top bezel.
  return renderer.getOrientation() == GfxRenderer::Orientation::Portrait ? 5 : 0;
#endif

  return 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int width = renderer.getTextWidth(fontId, text, style);
  if (width > screen.width) {
    const auto visible = renderer.truncatedText(fontId, text, screen.width, style);
    const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, visible.c_str(), style)) / 2;
    renderer.drawText(fontId, x, y, visible.c_str(), black, style);
    return;
  }
  const int x = screen.x + (screen.width - width) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

int UITheme::drawCenteredStatusRow(const GfxRenderer& renderer, const Rect screen, const int fontId, const int y,
                                   const char* label, const char* value) {
  const int labelWidth = renderer.getTextWidth(fontId, label, EpdFontFamily::BOLD);
  const int separatorWidth = renderer.getTextWidth(fontId, ":", EpdFontFamily::BOLD) + renderer.getSpaceWidth(fontId);
  const int width = labelWidth + separatorWidth + renderer.getTextWidth(fontId, value);
  if (width > screen.width) {
    // Long translations still keep this result separate from the next status.
    const int labelHeight =
        drawCenteredWrappedText(renderer, screen, fontId, y, label, 2, true, EpdFontFamily::BOLD, 4);
    return labelHeight + 4 +
           drawCenteredWrappedText(renderer, screen, fontId, y + labelHeight + 4, value, 2, true,
                                   EpdFontFamily::REGULAR, 4);
  }
  const int x = screen.x + (screen.width - width) / 2;
  renderer.drawText(fontId, x, y, label, true, EpdFontFamily::BOLD);
  renderer.drawText(fontId, x + labelWidth, y, ":", true, EpdFontFamily::BOLD);
  renderer.drawText(fontId, x + labelWidth + separatorWidth, y, value);
  return renderer.getLineHeight(fontId);
}

int UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, const Rect screen, const int fontId, int y,
                                     const char* text, const int maxLines, const bool black,
                                     const EpdFontFamily::Style style, const int lineSpacing) {
  const auto lines = renderer.wrappedText(fontId, text, screen.width, maxLines, style);
  return drawCenteredTextLines(renderer, screen, fontId, y, lines, black, style, lineSpacing);
}

int UITheme::drawCenteredWrappedTextAtCenter(const GfxRenderer& renderer, const Rect screen, const int fontId,
                                             const int y, const char* text, const int maxLines, const bool black,
                                             const EpdFontFamily::Style style, const int lineSpacing) {
  const auto lines = renderer.wrappedText(fontId, text, screen.width, maxLines, style);
  const int lineStep = renderer.getLineHeight(fontId) + lineSpacing;
  const int top = y - std::max(0, static_cast<int>(lines.size()) - 1) * lineStep / 2;
  return drawCenteredTextLines(renderer, screen, fontId, top, lines, black, style, lineSpacing);
}
