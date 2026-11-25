#pragma once

#include <AppCapabilities.h>
#include <EpdFontFamily.h>

#include <array>
#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const;
  const BaseTheme& getTheme() const { return *currentTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Draw one status row with a bold label and regular value. Returns its rendered height.
  static int drawCenteredStatusRow(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* label,
                                   const char* value);
  // Draw a word-wrapped text block centered within screen. Returns its rendered height.
  static int drawCenteredWrappedText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                                     int maxLines, bool black = true,
                                     EpdFontFamily::Style style = EpdFontFamily::REGULAR, int lineSpacing = 0);
  // Draw a word-wrapped text block centered around y's line baseline. Returns its rendered height.
  static int drawCenteredWrappedTextAtCenter(const GfxRenderer& renderer, Rect screen, int fontId, int y,
                                             const char* text, int maxLines, bool black = true,
                                             EpdFontFamily::Style style = EpdFontFamily::REGULAR, int lineSpacing = 0);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static bool supportsCoverGrid();
  static bool hasCoverGridHome();
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  // Returns the cache path for a generated thumbnail using the default 3:5
  // (width:height) aspect derived from coverHeight. Returns an empty string
  // when coverHeight is invalid.
  static std::string getCoverThumbPath(const std::string& coverBmpPath, int coverHeight);
  // Returns the cache path for a generated thumbnail at the requested cache-key
  // dimensions. coverBmpPath may be:
  // - a concrete path with no placeholders, returned unchanged;
  // - a dimensions template containing one [WIDTH] and one [HEIGHT] placeholder;
  // - a legacy height-only template containing one [HEIGHT] placeholder.
  // No scaling is done here. Returns an empty string for invalid dimensions or
  // unsupported placeholder templates.
  static std::string getCoverThumbPath(const std::string& coverBmpPath, int width, int height,
                                       bool allowLegacyFallback = true);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight(const GfxRenderer& renderer);
  static int getDisplayStatusBarFontId();
  static int getDisplayStatusBarTextHeight(const GfxRenderer& renderer);
  static int getDisplayStatusBarHeightIncrease();
  static int getReaderStatusBarFontId();
  static int getReaderStatusBarTextHeight(const GfxRenderer& renderer);
  static int getProgressBarHeight();
  static int getReaderStatusBarHeight(ReaderStatusBarPosition position, const GfxRenderer& renderer,
                                      const ReaderStatusBarConfig* overrideConfig = nullptr);
  static int getReaderProgressBarHeight(ReaderStatusBarPosition position);
  // Device-specific top offset for the clock, battery, and reserved status-bar lane.
  static int getTopStatusBarInset(const GfxRenderer& renderer);
  // Absolute screen origin shared by Home, menu headers, and the reader.
  static int getTopStatusBarY(const GfxRenderer& renderer);
  static int getButtonHintsReserve(const GfxRenderer& renderer);
  static int getButtonHintsBottomInset(const GfxRenderer& renderer);
  static int getHintSafeX(const GfxRenderer& renderer, int x, int width);
  static Rect getHeaderRect(const GfxRenderer& renderer, int height);
  static Rect getHeaderRect(const GfxRenderer& renderer, int height, const Rect& area);

 private:
  // Global text-size changes select an immutable variant, rather than rewriting
  // shared metrics or putting a ~300-byte copy on each nested render stack.
  // Theme replacement already runs under the activity render lock.
  std::array<ThemeMetrics, CROSSINK_APP_CAP_TOUCH ? 6 : 3> metricVariants{};
  void rebuildMetricVariants();
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
