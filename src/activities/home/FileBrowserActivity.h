#pragma once

#include <FileIndex.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Picker modes return their selected path via ActivityResult.
  enum class Mode { Books, PickFirmware, PickDirectory };

 private:
  // FreeInkApp hosts the file list (themed rows, icons, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  // Deletion
  void promptDeleteFile(const std::string& fullPath, const std::string& entry);
  void promptDeleteDirectory(const std::string& fullPath, const std::string& entry,
                             bool ignoreInitialConfirmRelease = false);
  void showDirectoryActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease = false);
  void pinSleepFavorite(const std::string& fullPath);
  void unpinSleepFavorite();
  bool isPinnedSleepFavorite(const std::string& fullPath) const;
  void setPreferredSleepFolder(const std::string& fullPath);
  void clearPreferredSleepFolder();
  bool isPreferredSleepFolder(const std::string& fullPath) const;
  bool isSleepFavoriteFolder(const std::string& fullPath) const;
  void pinBootFavorite(const std::string& fullPath);
  void unpinBootFavorite();
  bool isPinnedBootFavorite(const std::string& fullPath) const;
  void showFileActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease = false);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool showFileSelection = true;

  bool lockLongPressBack = false;
  bool longPressBackHandled = false;
  bool longPressConfirmHandled = false;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state
  static constexpr size_t INDEX_ROW_CACHE_SIZE = 32;
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;
  std::unique_ptr<FileIndex> fileIndex;
  std::unique_ptr<FileIndex::Entry> indexEntry;
  std::array<std::string, INDEX_ROW_CACHE_SIZE> indexCachedNames;
  std::array<size_t, INDEX_ROW_CACHE_SIZE> indexCachedRows{};
  bool usingIndex = false;
  bool fileListMemoryLimited = false;
  bool fileListReadFailed = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  // The current FreeInkUI action payload is int16_t. For larger folders the
  // renderer supplies a local row number and this records its absolute base.
  size_t actionWindowFirst = 0;
  freeink::ui::ListNav listNav;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSettingsEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void activateSelected();
  void navigateBack();
  void openSettings();

  // Data loading
  void clearIndexNameCache();
  void loadFiles();
  // Caller holds RenderLock while replacing the list backing storage together
  // with its associated navigation state.
  void loadFilesLocked();
  bool loadFilesIntoVector(size_t cap, bool& overflow);
  size_t entryCount() const;
  const char* entryNameAt(size_t row);
  void toggleHiddenFiles();
  size_t findEntry(const std::string& name);

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
