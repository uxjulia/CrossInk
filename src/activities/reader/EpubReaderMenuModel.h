#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class EpubReaderMenuAction : uint8_t {
  SELECT_CHAPTER,
  FOOTNOTES,
  GO_TO_PERCENT,
  AUTO_PAGE_TURN,
  ROTATE_SCREEN,
  SCREENSHOT,
  DISPLAY_QR,
  GO_HOME,
  SYNC,
  NEARBY_POSITION_SYNC,
  SEND_NEARBY_BOOK,
  DELETE_STATS,
  DELETE_CACHE,
  RESET_READING_PACE,
  READING_STATS,
  TOGGLE_COMPLETED,
  READER_OPTIONS,
  CONTROLS_OPTIONS,
  BOOKMARK_TOGGLE,
  VIEW_BOOKMARKS,
  DELETE_BOOKMARKS,
  SAVE_CLIPPING,
  VIEW_CLIPPINGS,
  LOOKUP,
  LOOKUP_HISTORY,
  SET_BOOK_DICTIONARY,
  STATUS_BAR_SETTINGS,
};

enum class ReaderDrawerTab : uint8_t { Font = 0, Layout = 1, More = 2, Location = 3, Settings = 4, Count };

constexpr size_t READER_DRAWER_TAB_COUNT = static_cast<size_t>(ReaderDrawerTab::Count);
constexpr uint16_t READER_AUTO_PAGE_TURN_MIN_SECONDS = 5;
constexpr uint16_t READER_AUTO_PAGE_TURN_MAX_SECONDS = 120;

enum class ReaderDrawerPane : uint8_t {
  Root = 0,
  ReaderFont,
  FontFamily,
  Spacing,
  Margins,
  Chapters,
  Percent,
  AutoPageTurn,
  Dictionary,
  DictionaryFont,
  EnumOptions,
};

enum class ReaderDrawerCatalogItem : uint8_t {
  ReaderFont,
  DictionaryFont,
  Spacing,
  TextAa,
  Bionic,
  GuideDots,
  Margins,
  Orientation,
  Alignment,
  Hyphenation,
  PublisherPages,
  ExtraSpacing,
  ForceIndents,
  EmbeddedStyle,
  Images,
  SelectChapter,
  GoToPercent,
  BookmarkToggle,
  ViewBookmarks,
  Screenshot,
  DisplayQr,
  Footnotes,
  Lookup,
  LookupHistory,
  SaveClipping,
  ViewClippings,
  DeleteBookmarks,
  StatusBar,
  BookDictionary,
  RenderMode,
  IndexingMethod,
  ToggleCompleted,
  Controls,
  ResetReadingPace,
  DeleteCache,
  DeleteStats,
  AutoPageTurn,
  FontFamily,
  FontSize,
  DictionaryFontFamily,
  DictionaryFontSize,
};

struct ReaderDrawerAvailability {
  bool hasFootnotes = false;
  bool hasDictionary = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool showReadingPaceReset = false;
};

struct ReaderDrawerTabCatalog {
  std::array<ReaderDrawerCatalogItem, 12> items{};
  uint8_t count = 0;

  constexpr void add(const ReaderDrawerCatalogItem item) { items[count++] = item; }
};

using ReaderDrawerCatalog = std::array<ReaderDrawerTabCatalog, READER_DRAWER_TAB_COUNT>;

constexpr ReaderDrawerCatalog makeReaderDrawerCatalog(const ReaderDrawerAvailability available) {
  ReaderDrawerCatalog catalog{};
  auto& font = catalog[static_cast<size_t>(ReaderDrawerTab::Font)];
  font.add(ReaderDrawerCatalogItem::ReaderFont);
  font.add(ReaderDrawerCatalogItem::DictionaryFont);
  font.add(ReaderDrawerCatalogItem::Spacing);
  font.add(ReaderDrawerCatalogItem::TextAa);
  font.add(ReaderDrawerCatalogItem::Bionic);
  font.add(ReaderDrawerCatalogItem::GuideDots);

  auto& layout = catalog[static_cast<size_t>(ReaderDrawerTab::Layout)];
  layout.add(ReaderDrawerCatalogItem::Margins);
  layout.add(ReaderDrawerCatalogItem::Orientation);
  layout.add(ReaderDrawerCatalogItem::Alignment);
  layout.add(ReaderDrawerCatalogItem::Images);
  layout.add(ReaderDrawerCatalogItem::Hyphenation);
  layout.add(ReaderDrawerCatalogItem::PublisherPages);
  layout.add(ReaderDrawerCatalogItem::ExtraSpacing);
  layout.add(ReaderDrawerCatalogItem::ForceIndents);
  layout.add(ReaderDrawerCatalogItem::EmbeddedStyle);

  auto& more = catalog[static_cast<size_t>(ReaderDrawerTab::More)];
  if (available.hasDictionary) {
    more.add(ReaderDrawerCatalogItem::Lookup);
    more.add(ReaderDrawerCatalogItem::LookupHistory);
  }
  more.add(ReaderDrawerCatalogItem::SelectChapter);
  more.add(ReaderDrawerCatalogItem::GoToPercent);
  more.add(ReaderDrawerCatalogItem::AutoPageTurn);
  if (available.hasFootnotes) more.add(ReaderDrawerCatalogItem::Footnotes);

  auto& location = catalog[static_cast<size_t>(ReaderDrawerTab::Location)];
  location.add(ReaderDrawerCatalogItem::BookmarkToggle);
  if (available.hasBookmarks) {
    location.add(ReaderDrawerCatalogItem::ViewBookmarks);
    location.add(ReaderDrawerCatalogItem::DeleteBookmarks);
  }
  location.add(ReaderDrawerCatalogItem::SaveClipping);
  if (available.hasClippings) location.add(ReaderDrawerCatalogItem::ViewClippings);
  location.add(ReaderDrawerCatalogItem::Screenshot);
  location.add(ReaderDrawerCatalogItem::DisplayQr);

  auto& settings = catalog[static_cast<size_t>(ReaderDrawerTab::Settings)];
  settings.add(ReaderDrawerCatalogItem::StatusBar);
  settings.add(ReaderDrawerCatalogItem::Controls);
  settings.add(ReaderDrawerCatalogItem::BookDictionary);
  settings.add(ReaderDrawerCatalogItem::RenderMode);
  settings.add(ReaderDrawerCatalogItem::IndexingMethod);
  settings.add(ReaderDrawerCatalogItem::ToggleCompleted);
  if (available.showReadingPaceReset) settings.add(ReaderDrawerCatalogItem::ResetReadingPace);
  settings.add(ReaderDrawerCatalogItem::DeleteCache);
  settings.add(ReaderDrawerCatalogItem::DeleteStats);
  return catalog;
}

constexpr bool shouldReopenTouchReaderDrawer(const bool reopenDrawer, const bool hasTouchHardware) {
  return reopenDrawer && hasTouchHardware;
}

constexpr bool readerDrawerStepChangesSettings(const ReaderDrawerPane pane) {
  return pane == ReaderDrawerPane::Spacing || pane == ReaderDrawerPane::Margins ||
         pane == ReaderDrawerPane::AutoPageTurn;
}

constexpr bool readerDrawerSliderPreviewsText(const ReaderDrawerPane pane) {
  return pane == ReaderDrawerPane::Spacing || pane == ReaderDrawerPane::Margins;
}

// These are the only panes that place two annotated sliders in one drawer.
// Keep their extra landscape height separate from the compact list panes.
constexpr bool readerDrawerNeedsTallLandscapeSheet(const ReaderDrawerPane pane) {
  return readerDrawerSliderPreviewsText(pane);
}

constexpr bool isReaderDrawerRowFocused(const bool buttonFocusActive, const int16_t selectedIndex,
                                        const int16_t rowIndex) {
  return buttonFocusActive && selectedIndex == rowIndex;
}

constexpr int16_t readerDrawerFocusedWindowIndex(const bool buttonFocusActive, const int16_t selectedIndex,
                                                 const int16_t topIndex) {
  return buttonFocusActive ? static_cast<int16_t>(selectedIndex - topIndex) : -1;
}

constexpr int readerDrawerVisibleRows(const int16_t listHeight, const int16_t rowHeight, const int16_t rowGap) {
  return rowHeight + rowGap > 0 ? (listHeight + rowGap) / (rowHeight + rowGap) : 1;
}

constexpr int16_t readerDrawerListHeightForRows(const int rows, const int16_t rowHeight, const int16_t rowGap) {
  if (rows <= 0) return 0;
  return static_cast<int16_t>(rows * rowHeight + (rows - 1) * rowGap);
}

struct ReaderDrawerState {
  ReaderDrawerTab tab = ReaderDrawerTab::Font;
  ReaderDrawerPane pane = ReaderDrawerPane::Root;
  int16_t selectedIndex = 0;
  std::array<int16_t, READER_DRAWER_TAB_COUNT> rootTopIndex{};
  int16_t paneTopIndex = 0;
  int16_t pendingFontIndex = -1;
};

inline void restoreReaderDrawerScroll(ReaderDrawerState& state, const int16_t scrollPosition) {
  if (state.pane == ReaderDrawerPane::Root) {
    state.rootTopIndex[static_cast<size_t>(state.tab)] = scrollPosition;
  } else {
    state.paneTopIndex = scrollPosition;
  }
}

struct ReaderSettingsDraft {
  uint8_t fontFamily = 0;
  uint8_t readerFontPointSize = 0;
  std::array<char, 64> sdFontFamilyName{};
  uint8_t lineHeightPercent = 0;
  uint8_t wordSpacing = 0;
  uint8_t screenMarginVertical = 0;
  uint8_t screenMarginHorizontal = 0;
  uint8_t orientation = 0;
  uint8_t paragraphAlignment = 0;
  uint8_t textAntiAliasing = 0;
  uint8_t bionicReadingEnabled = 0;
  uint8_t guideReadingEnabled = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t publisherPageNumbers = 0;
  uint8_t extraParagraphSpacing = 0;
  uint8_t forceParagraphIndents = 0;
  uint8_t embeddedStyle = 0;
  uint8_t imageRendering = 0;
  uint8_t epubRenderMode = 0;
  uint8_t indexingMethod = 0;
};

enum class ReaderSettingsChangeMask : uint8_t {
  None = 0,
  Preview = 1 << 0,
  Relayout = 1 << 1,
  Orientation = 1 << 2,
  NonLayout = 1 << 3,
};

constexpr ReaderSettingsChangeMask operator|(const ReaderSettingsChangeMask left,
                                             const ReaderSettingsChangeMask right) {
  return static_cast<ReaderSettingsChangeMask>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr bool hasReaderSettingsChange(const ReaderSettingsChangeMask mask, const ReaderSettingsChangeMask change) {
  return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(change)) != 0;
}
