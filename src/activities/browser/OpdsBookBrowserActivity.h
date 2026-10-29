#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <OpdsParser.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    DESCRIPTION,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
#ifdef SIMULATOR
  friend struct OpdsCatalogSmokeTest;
#endif

  // Own only the selected book metadata while the full catalog is released.
  // The extra title copy is parser-bounded to 160 bytes and must outlive an
  // overwrite callback; URL/filename storage was already needed for downloads.
  struct DownloadRequest {
    std::string url;
    std::string title;
    std::string filename;
  };

  // FreeInkUI app runtime for the browsing screen: owns the interaction table,
  // routes touch snapshots, and dispatches row/search actions to the static
  // handlers below. 24 interaction slots cover the densest page (Small scale,
  // ~12 rows) plus the header's search button, with headroom.
  using UiApp = freeink::ui::FreeInkApp<24, 6>;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::unique_ptr<OpdsEntry[]> entries;
  static constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = MAX_OPDS_FEED_ENTRIES + 2;
  // 832 bytes owned by this activity; reused each render rather than allocated on the task stack.
  std::array<std::array<char, 16>, OPDS_BROWSER_ENTRY_CAPACITY> countLabels{};
  size_t entryCount = 0;
  bool hasPrevPageRow = false;
  bool hasNextPageRow = false;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  // Wrapped only when a preview opens or its available width changes.
  std::vector<std::string> descriptionLines;
  int descriptionWidth = 0;
  int descriptionTop = 0;
  int descriptionRows = 1;
  int topIndex = 0;  // viewport scroll position, decoupled from the selection
  // Read by HttpDownloader between chunks; set by the Cancel button handler or
  // a Back press, both pumped from the download's progress callback.
  bool cancelDownload = false;
  // A blocking downloader consumes the one-shot Home event itself. Defer the
  // activity exit until HttpDownloader has unwound and closed the partial file.
  bool goHomeAfterCancel = false;
  bool catalogReleasedForDownload = false;
  int downloadSelectorIndex = 0;
  int downloadTopIndex = 0;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, bool withSearch);
  void buildBrowsingScreen(UiApp::ScreenType& screen);
  static void provideRow(void* user, uint16_t index, freeink::ui::ListItem& item);
  static void onDescriptionEvent(const freeink::ui::ActionEvent& event, void* user);
  void scrollDescription(int direction);
  void buildDescriptionScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void fetchFeed(const std::string& path);
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&& entry);
  void navigateToEntry(const OpdsEntry& entry, bool pageLink);
  void navigateBack();
  bool loadPublication(OpdsEntry& book, const std::string& url);
  void openPublication(OpdsEntry& book);
  void requestDownload(const OpdsEntry& book);
  void confirmDownload(DownloadRequest request, const std::string& destination);
  void downloadBook(DownloadRequest request, const std::string& approvedPath = "");
  void finishDownload(DownloadRequest request, HttpDownloader::DownloadError result, const std::string& resolvedPath);
  void restoreCatalogAfterDownload();
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
