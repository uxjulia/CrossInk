#pragma once

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

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
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
  static std::string getCoverThumbPath(const std::string& coverBmpPath, int width, int height);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
